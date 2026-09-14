// Task 6 RED→GREEN 测试：latest-wins RawData Worker 的主线程封装。
//
// 用 fake Worker（不真起线程）注入 RawDataDecoder，断言：
//  1. definitions 必须先注册：未 setDefinitions 时 decode 结果携带 error。
//  2. buffer 所有权：主线程送出的是 payload.slice() 的副本 buffer 且被 transfer；
//     原 payload buffer 绝不进 transfer list（byteLength 不为 0、仍可读）。
//  3. 结果携带相同 identity 元组（generation/channel/seq）+ text/lines/error。
//  4. latest-wins：同通道新 seq 使旧 Promise 结果标记 stale（不 reject、不抛）。
//  5. generation / channel 变化后回包的旧结果标记 stale（不 reject）。
//  6. dispose：terminate Worker 并拒绝所有 pending。
import { describe, it, expect } from "vitest";
import { RawDataDecoder } from "./rawDataDecoder";
import type { RawDataDefs } from "../types";

// --- fake Worker ------------------------------------------------------------

interface PostCall {
  message: unknown;
  transfer: Transferable[];
}

// 模拟 Worker：记录 postMessage 调用（消息 + transfer list），
// 允许测试手动触发 onmessage 回包与断言 terminate。
class FakeWorker {
  posts: PostCall[] = [];
  terminated = false;
  onmessage: ((ev: MessageEvent) => void) | null = null;
  onerror: ((ev: unknown) => void) | null = null;

  postMessage(message: unknown, transfer?: Transferable[]): void {
    this.posts.push({ message, transfer: transfer ?? [] });
  }

  terminate(): void {
    this.terminated = true;
  }

  // 测试辅助：模拟 Worker 回包到主线程。
  emit(data: unknown): void {
    this.onmessage?.({ data } as MessageEvent);
  }

  // 取最近一次 decode 请求的消息体。
  lastDecodeMsg(): Record<string, unknown> {
    const decodes = this.posts.filter(
      (p) => (p.message as { type?: string }).type === "decode",
    );
    return decodes[decodes.length - 1].message as Record<string, unknown>;
  }
}

const EMPTY_DEFS: RawDataDefs = { rawData: [], schemas: [] };

function makeDecoder(): { dec: RawDataDecoder; w: FakeWorker } {
  const w = new FakeWorker();
  const dec = new RawDataDecoder(() => w as unknown as Worker);
  return { dec, w };
}

// 回放一个 decode 回包，identity 取自最近一次 decode 请求。
function respondLatest(
  w: FakeWorker,
  extra: { text?: string; lines?: string[]; error?: string },
): void {
  const msg = w.lastDecodeMsg();
  w.emit({
    type: "decoded",
    id: msg.id,
    generation: msg.generation,
    channel: msg.channel,
    seq: msg.seq,
    text: extra.text ?? "",
    lines: extra.lines ?? [],
    error: extra.error,
  });
}

describe("RawDataDecoder（latest-wins / identity / buffer 所有权）", () => {
  it("未 setDefinitions 前 decode 结果携带 error（definitions 必须先注册）", async () => {
    const { dec } =makeDecoder();
    const res = await dec.decode({
      generation: 1,
      channel: "chA",
      seq: 1,
      schemaId: "1",
      messageType: "Foo",
      payload: new Uint8Array([1, 2, 3]),
    });
    expect(res.error).toBeTruthy();
    expect(res.generation).toBe(1);
    expect(res.channel).toBe("chA");
    expect(res.seq).toBe(1);
    dec.dispose();
  });

  it("transfer 的是 payload.slice() 副本 buffer，原 payload buffer 不被 detach", async () => {
    const { dec, w } = makeDecoder();
    dec.setDefinitions(EMPTY_DEFS);

    const payload = new Uint8Array([10, 20, 30, 40]);
    const origBuffer = payload.buffer;
    const p = dec.decode({
      generation: 5,
      channel: "chB",
      seq: 7,
      schemaId: "2",
      messageType: "Bar",
      payload,
    });

    const call = w.posts.find(
      (c) => (c.message as { type?: string }).type === "decode",
    )!;
    const sentPayload = (call.message as { payload: Uint8Array }).payload;

    // 送出的必须是内容相同但 buffer 不同的副本。
    expect(Array.from(sentPayload)).toEqual([10, 20, 30, 40]);
    expect(sentPayload.buffer).not.toBe(origBuffer);

    // transfer list 只含副本 buffer，绝不含原 buffer。
    expect(call.transfer).toContain(sentPayload.buffer);
    expect(call.transfer).not.toContain(origBuffer);

    // 原 payload 未被 detach：仍可读、byteLength 不为 0。
    expect(payload.byteLength).toBe(4);
    expect(Array.from(payload)).toEqual([10, 20, 30, 40]);

    respondLatest(w, { text: "ok", lines: ["ok"] });
    const res = await p;
    expect(res.error).toBeUndefined();
    expect(res.text).toBe("ok");
    dec.dispose();
  });

  it("成功结果携带相同 identity 元组（generation/channel/seq）+ text/lines", async () => {
    const { dec, w } = makeDecoder();
    dec.setDefinitions(EMPTY_DEFS);
    const p = dec.decode({
      generation: 2,
      channel: "chC",
      seq: 42,
      schemaId: "1",
      messageType: "T",
      payload: new Uint8Array([1]),
    });
    respondLatest(w, { text: "a: 1", lines: ["a: 1"] });
    const res = await p;
    expect(res).toMatchObject({
      generation: 2,
      channel: "chC",
      seq: 42,
      text: "a: 1",
      lines: ["a: 1"],
    });
    expect(res.stale).toBeFalsy();
    dec.dispose();
  });

  it("同通道更新的 seq 使旧结果 stale（不 reject、不抛 UI 错误）", async () => {
    const { dec, w } = makeDecoder();
    dec.setDefinitions(EMPTY_DEFS);

    const pOld = dec.decode({
      generation: 1,
      channel: "chX",
      seq: 1,
      schemaId: "1",
      messageType: "T",
      payload: new Uint8Array([1]),
    });
    const oldMsg = w.lastDecodeMsg();

    // 同通道发起更新的 seq。
    const pNew = dec.decode({
      generation: 1,
      channel: "chX",
   seq: 2,
      schemaId: "1",
      messageType: "T",
      payload: new Uint8Array([2]),
    });

    // 旧请求回包：应被标记 stale。
    w.emit({
      type: "decoded",
      id: oldMsg.id,
      generation: 1,
      channel: "chX",
      seq: 1,
      text: "old",
      lines: ["old"],
    });
    const oldRes = await pOld;
    expect(oldRes.stale).toBe(true);
    expect(oldRes.error).toBeUndefined();

    // 新请求回包：非 stale。
    respondLatest(w, { text: "new", lines: ["new"] });
    const newRes = await pNew;
    expect(newRes.stale).toBeFalsy();
    expect(newRes.text).toBe("new");
    dec.dispose();
  });

  it("generation 变化后回包的旧结果 stale", async () => {
    const { dec, w } = makeDecoder();
    dec.setDefinitions(EMPTY_DEFS);
    const p = dec.decode({
      generation: 1,
      channel: "chY",
      seq: 1,
      schemaId: "1",
      messageType: "T",
      payload: new Uint8Array([1]),
    });
    // 换源：generation 前进。
    dec.setGeneration(2);
    respondLatest(w, { text: "stale-gen", lines: ["stale-gen"] });
    const res = await p;
    expect(res.stale).toBe(true);
    dec.dispose();
  });

  it("dispose 拒绝所有 pending 并 terminate Worker", async () => {
    const { dec, w } = makeDecoder();
    dec.setDefinitions(EMPTY_DEFS);
    const p = dec.decode({
      generation: 1,
      channel: "chZ",
      seq: 1,
      schemaId: "1",
      messageType: "T",
      payload: new Uint8Array([1]),
    });
    dec.dispose();
    expect(w.terminated).toBe(true);
    await expect(p).rejects.toThrow();
    // dispose 后再 decode 直接 reject。
    await expect(
      dec.decode({
        generation: 1,
        channel: "chZ",
        seq: 2,
        schemaId: "1",
        messageType: "T",
        payload: new Uint8Array([1]),
      }),
    ).rejects.toThrow();
  });

  it("setDefinitions 向 Worker 发送 defs 消息（串行：先注册后 decode 生效）", () => {
    const { dec, w } = makeDecoder();
    const defs: RawDataDefs = {
      rawData: [],
      schemas: [{ id: "1", encoding: "protobuf", dataBase64: "" }],
    };
    dec.setDefinitions(defs);
    const defsMsg = w.posts.find(
      (c) => (c.message as { type?: string }).type === "defs",
    );
    expect(defsMsg).toBeTruthy();
    expect((defsMsg!.message as { defs: RawDataDefs }).defs).toEqual(defs);
    dec.dispose();
  });
});