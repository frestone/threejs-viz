// Task 8：ThreeEngine RawData 接线逻辑单测（RED→GREEN）。
// 用 fake decoder / fake store 注入，验证 spec §7 步骤5-8 + §11 订阅默认关闭。
import { describe, it, expect, vi } from "vitest";
import { createRawDataWiring, type DecoderLike, type StoreLike } from "./rawDataWiring";
import type { RawDataDefs } from "../types";
import type { RawDataDecodeResult } from "./rawDataDecoder";

const DEFS: RawDataDefs = {
  rawData: [
    { id: "chA", topic: "/a", label: "A", available: true, messageType: "pkg.A", schemaId: "1" },
    { id: "chB", topic: "/b", label: "B", available: true, messageType: "pkg.B", schemaId: "2" },
  ],
  schemas: [
    { id: "1", encoding: "protobuf", dataBase64: "AA==" },
    { id: "2", encoding: "protobuf", dataBase64: "BB==" },
  ],
};

function makeDecoder(overrides?: Partial<DecoderLike>): DecoderLike & {
  setDefinitions: ReturnType<typeof vi.fn>;
  setGeneration: ReturnType<typeof vi.fn>;
  decode: ReturnType<typeof vi.fn>;
  dispose: ReturnType<typeof vi.fn>;
} {
  return {
    setDefinitions: vi.fn(),
    setGeneration: vi.fn(),
    decode: vi.fn(async (req): Promise<RawDataDecodeResult> => ({
      generation: req.generation,
      channel: req.channel,
      seq: req.seq,
      text: "ok",
      lines: ["ok"],
    })),
    dispose: vi.fn(),
    ...overrides,
  } as never;
}

function makeStore(payload = new Uint8Array([1, 2, 3]), gen = 0, seq = 0): StoreLike {
  return {
    get: (channel: string) =>
      channel === "chA" || channel === "chB"
        ? { tSec: 0, gen, kind: "raw" as const, seq, payload }
        : undefined,
  };
}

describe("createRawDataWiring", () => {
  it("收到 defs → decoder.setDefinitions 被调用并进入 panel state", () => {
    const decoder = makeDecoder();
    const w = createRawDataWiring({ decoder, store: makeStore(), currentGeneration: () => 0 });
    w.handleDefs(DEFS);
    expect(decoder.setDefinitions).toHaveBeenCalledWith(DEFS);
    expect(w.getState().selectableChannels).toEqual(["chA", "chB"]);
  });

  it("订阅默认关闭：未选通道/未开启时收到 raw 帧不 decode", () => {
    const decoder = makeDecoder();
    const w = createRawDataWiring({ decoder, store: makeStore(), currentGeneration: () => 0 });
    w.handleDefs(DEFS);
    w.onRawFrame("chA");
    expect(decoder.decode).not.toHaveBeenCalled();
  });

  it("首次选择通道时立即解码已缓存的 protobuf 帧", () => {
    const decoder = makeDecoder();
    const w = createRawDataWiring({
      decoder,
      store: makeStore(new Uint8Array([9]), 0, 5),
      currentGeneration: () => 0,
    });
    w.handleDefs(DEFS);

    w.subscribe("chA", true);

    expect(decoder.decode).toHaveBeenCalledTimes(1);
    expect(decoder.decode).toHaveBeenCalledWith(expect.objectContaining({
      channel: "chA",
      seq: 5,
      payload: new Uint8Array([9]),
    }));
  });

  it("订阅开启+选中通道+raw 帧 → decode 被调用 → panel ready+lines", async () => {
    const decoder = makeDecoder();
    const w = createRawDataWiring({ decoder, store: makeStore(new Uint8Array([9]), 0, 5), currentGeneration: () => 0 });
    w.handleDefs(DEFS);
    w.subscribe("chA", true);
    w.onRawFrame("chA");
    expect(decoder.decode).toHaveBeenCalledTimes(1);
    const arg = decoder.decode.mock.calls[0][0];
    expect(arg.channel).toBe("chA");
    expect(arg.messageType).toBe("pkg.A");
    expect(arg.schemaId).toBe("1");
    await Promise.resolve();
    await Promise.resolve();
    expect(w.getState().status).toBe("ready");
    expect(w.getState().lines).toEqual(["ok"]);
  });

  it("播放中 raw 帧 wire seq 恒为 0 时仍按时间连续解码最新帧", () => {
    const decoder = makeDecoder();
    let entry = {
      tSec: 1,
      gen: 0,
      kind: "raw" as const,
      seq: 0,
      payload: new Uint8Array([1]),
    };
    const store: StoreLike = { get: () => entry };
    const w = createRawDataWiring({ decoder, store, currentGeneration: () => 0 });
    w.handleDefs(DEFS);
    w.subscribe("chA", true);

    entry = { ...entry, tSec: 1.04, payload: new Uint8Array([2]) };
    w.onRawFrame("chA");

    expect(decoder.decode).toHaveBeenCalledTimes(2);
    expect(decoder.decode.mock.calls[0][0].seq).toBe(1);
    expect(decoder.decode.mock.calls[1][0].seq).toBe(2);
    expect(decoder.decode.mock.calls[1][0].payload).toEqual(new Uint8Array([2]));
  });

  it("播放中较早的 raw seq=0 异步结果不会覆盖较新帧", async () => {
    const pending: Array<(result: RawDataDecodeResult) => void> = [];
    const decoder = makeDecoder({
      decode: vi.fn(() => new Promise<RawDataDecodeResult>((resolve) => pending.push(resolve))),
    });
    let entry = {
      tSec: 1,
      gen: 0,
      kind: "raw" as const,
      seq: 0,
      payload: new Uint8Array([1]),
    };
    const store: StoreLike = { get: () => entry };
    const w = createRawDataWiring({ decoder, store, currentGeneration: () => 0 });
    w.handleDefs(DEFS);
    w.subscribe("chA", true);

    entry = { ...entry, tSec: 1.04, payload: new Uint8Array([2]) };
    w.onRawFrame("chA");
    pending[1]({ generation: 0, channel: "chA", seq: 2, text: "new", lines: ["new"] });
    await Promise.resolve();
    await Promise.resolve();
    expect(w.getState().text).toBe("new");

    pending[0]({ generation: 0, channel: "chA", seq: 1, text: "old", lines: ["old"] });
    await Promise.resolve();
    await Promise.resolve();
    expect(w.getState().text).toBe("new");
  });

  it("暂停时收到 raw 帧不触发 decode", () => {
    const decoder = makeDecoder();
    const w = createRawDataWiring({
      decoder,
      store: makeStore(),
      currentGeneration: () => 0,
      isPaused: () => true,
    });
    w.handleDefs(DEFS);
    w.subscribe("chA", true);
    w.onRawFrame("chA");
    expect(decoder.decode).not.toHaveBeenCalled();
  });

  it("暂停前已提交、暂停后返回的 decode 结果不更新 panel", async () => {
    let paused = false;
    let resolveDecode!: (result: RawDataDecodeResult) => void;
    const decoder = makeDecoder({
      decode: vi.fn(() => new Promise<RawDataDecodeResult>((resolve) => {
        resolveDecode = resolve;
      })),
    });
    const w = createRawDataWiring({
      decoder,
      store: makeStore(new Uint8Array([9]), 0, 5),
      currentGeneration: () => 0,
      isPaused: () => paused,
    });
    w.handleDefs(DEFS);
    w.subscribe("chA", true);
    w.onRawFrame("chA");
    expect(w.getState().status).toBe("decoding");

    paused = true;
    w.setPaused(true);
    expect(w.getState().status).toBe("idle");
    resolveDecode({ generation: 0, channel: "chA", seq: 5, text: "late", lines: ["late"] });
    await Promise.resolve();
    await Promise.resolve();

    expect(w.getState().text).toBe("");
    expect(w.getState().lines).toEqual([]);
    expect(w.getState().status).toBe("idle");
  });

  it("面板隐藏后停止新解码、取消等待态并忽略在途结果", async () => {
    let resolveDecode!: (result: RawDataDecodeResult) => void;
    const decoder = makeDecoder({
      decode: vi.fn(() => new Promise<RawDataDecodeResult>((resolve) => {
        resolveDecode = resolve;
      })),
    });
    const w = createRawDataWiring({
      decoder,
      store: makeStore(new Uint8Array([9]), 0, 5),
      currentGeneration: () => 0,
    });
    w.handleDefs(DEFS);
    w.subscribe("chA", true);
    w.onRawFrame("chA");
    expect(w.getState().status).toBe("decoding");

    w.setVisible(false);
    expect(w.getState().status).toBe("idle");
    w.onRawFrame("chA");
    expect(decoder.decode).toHaveBeenCalledTimes(1);

    resolveDecode({ generation: 0, channel: "chA", seq: 5, text: "late", lines: ["late"] });
    await Promise.resolve();
    await Promise.resolve();
    expect(w.getState().text).toBe("");
  });

  it("面板重新显示后立即解码当前通道缓存帧", async () => {
    const decoder = makeDecoder();
    const w = createRawDataWiring({
      decoder,
      store: makeStore(new Uint8Array([9]), 0, 5),
      currentGeneration: () => 0,
    });
    w.handleDefs(DEFS);
    w.subscribe("chA", true);
    decoder.decode.mockClear();
    w.setVisible(false);
    w.onRawFrame("chA");
    expect(decoder.decode).not.toHaveBeenCalled();

    w.setVisible(true);
    expect(decoder.decode).toHaveBeenCalledTimes(1);
    await Promise.resolve();
    await Promise.resolve();
    expect(w.getState().status).toBe("ready");
  });

  it("非当前选中通道的 raw 帧不 decode", () => {
    const decoder = makeDecoder();
    const w = createRawDataWiring({ decoder, store: makeStore(), currentGeneration: () => 0 });
    w.handleDefs(DEFS);
    w.subscribe("chA", true);
    decoder.decode.mockClear();
    w.onRawFrame("chB");
    expect(decoder.decode).not.toHaveBeenCalled();
  });

  it("换通道：清旧文本并立即解码新通道缓存帧", async () => {
    const decoder = makeDecoder({
      decode: vi.fn(async (req) => ({
        generation: req.generation,
        channel: req.channel,
        seq: req.seq,
        text: req.channel === "chA" ? "A text" : "B text",
        lines: [req.channel === "chA" ? "A text" : "B text"],
      })),
    });
    const store: StoreLike = {
      get: (channel) => ({
        tSec: 0,
        gen: 0,
        kind: "raw",
        seq: channel === "chA" ? 1 : 2,
        payload: new Uint8Array([channel === "chA" ? 1 : 2]),
      }),
    };
    const w = createRawDataWiring({ decoder, store, currentGeneration: () => 0 });
    w.handleDefs(DEFS);
    w.subscribe("chA", true);
    w.onRawFrame("chA");
    await Promise.resolve();
    await Promise.resolve();
    expect(w.getState().text).toBe("A text");

    w.subscribe("chB", true);
    expect(w.getState().selected).toBe("chB");
    expect(w.getState().text).toBe("");
    expect(decoder.decode).toHaveBeenLastCalledWith(expect.objectContaining({ channel: "chB", seq: 2 }));

    await Promise.resolve();
    await Promise.resolve();
    expect(w.getState().text).toBe("B text");
  });

  it("换通道后旧通道的延迟结果不会覆盖新通道文本", async () => {
    let resolveA!: (result: RawDataDecodeResult) => void;
    const decoder = makeDecoder({
      decode: vi.fn((req) => req.channel === "chA"
        ? new Promise<RawDataDecodeResult>((resolve) => { resolveA = resolve; })
        : Promise.resolve({
          generation: req.generation,
          channel: req.channel,
          seq: req.seq,
          text: "B text",
          lines: ["B text"],
        })),
    });
    const store: StoreLike = {
      get: (channel) => ({
        tSec: 0,
        gen: 0,
        kind: "raw",
        seq: channel === "chA" ? 1 : 2,
        payload: new Uint8Array([1]),
      }),
    };
    const w = createRawDataWiring({ decoder, store, currentGeneration: () => 0 });
    w.handleDefs(DEFS);
    w.subscribe("chA", true);
    w.onRawFrame("chA");
    w.subscribe("chB", true);
    await Promise.resolve();
    await Promise.resolve();
    expect(w.getState().text).toBe("B text");

    resolveA({ generation: 0, channel: "chA", seq: 1, text: "late A", lines: ["late A"] });
    await Promise.resolve();
    await Promise.resolve();
    expect(w.getState().text).toBe("B text");
  });

  it("关闭订阅：清 panel 选择并停止 decode", () => {
    const decoder = makeDecoder();
    const w = createRawDataWiring({ decoder, store: makeStore(), currentGeneration: () => 0 });
    w.handleDefs(DEFS);
    w.subscribe("chA", true);
    decoder.decode.mockClear();
   w.subscribe("chA", false);
    w.onRawFrame("chA");
    expect(decoder.decode).not.toHaveBeenCalled();
    expect(w.getState().selected).toBeNull();
  });

  it("clearCache：decoder.setGeneration + panel 清理", () => {
    const decoder = makeDecoder();
    const w = createRawDataWiring({ decoder, store: makeStore(), currentGeneration: () => 7 });
    w.handleDefs(DEFS);
    w.subscribe("chA", true);
    w.onClearCache();
    expect(decoder.setGeneration).toHaveBeenCalledWith(7);
    // panel 内容清空（保留通道列表）
    expect(w.getState().lines).toEqual([]);
    expect(w.getState().status).toBe("idle");
  });

  it("错误隔离：decode reject → panel error，不抛出", async () => {
    const decoder = makeDecoder({
      decode: vi.fn(() => Promise.reject(new Error("boom"))),
    });
    const w = createRawDataWiring({ decoder, store: makeStore(new Uint8Array([9]), 0, 5), currentGeneration: () => 0 });
    w.handleDefs(DEFS);
    w.subscribe("chA", true);
    expect(() => w.onRawFrame("chA")).not.toThrow();
    await Promise.resolve();
    await Promise.resolve();
    expect(w.getState().status).toBe("error");
    expect(w.getState().error).toContain("boom");
  });

  it("decode 结果 error 字段 → panel error", async () => {
    const decoder = makeDecoder({
      decode: vi.fn(async (req) => ({
        generation: req.generation, channel: req.channel, seq: req.seq,
        text: "", lines: [], error: "bad schema",
      })),
    });
    const w = createRawDataWiring({ decoder, store: makeStore(new Uint8Array([9]), 0, 5), currentGeneration: () => 0 });
    w.handleDefs(DEFS);
    w.subscribe("chA", true);
    w.onRawFrame("chA");
    await Promise.resolve();
    await Promise.resolve();
    expect(w.getState().status).toBe("error");
    expect(w.getState().error).toContain("bad schema");
  });

  it("panel 更新通知回调被触发", () => {
    const decoder = makeDecoder();
    const onUpdate = vi.fn();
    const w = createRawDataWiring({ decoder, store: makeStore(), currentGeneration: () => 0, onPanelUpdate: onUpdate });
    w.handleDefs(DEFS);
    expect(onUpdate).toHaveBeenCalled();
  });

  it("dispose：decoder.dispose 被调用", () => {
    const decoder = makeDecoder();
    const w = createRawDataWiring({ decoder, store: makeStore(), currentGeneration: () => 0 });
    w.dispose();
    expect(decoder.dispose).toHaveBeenCalled();
  });
});