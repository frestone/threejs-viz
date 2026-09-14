// Task 6：RawData 解码的主线程侧封装（latest-wins / identity / buffer 所有权）。
//
// 单 module Worker（rawDataWorker.ts）串行处理 registry 更新与 decode；本类负责：
//  1. buffer 所有权：主线程收到的 payload 属于 BigDataStore，绝不能转移其 buffer。
//     发送前 payload.slice() 得到独立副本，只把副本 buffer 放进 transfer list；
//     原 payload 因此不被 detach，BigDataStore 侧仍可安全复用。
//  2. latest-wins / identity：维护 epoch（generation）与每通道最新 seq。发起 decode
//     时记录 (generation, channel, seq)；Worker 回包后再次核对——若 generation 已变、
//     或该通道已有更新 seq，则结果 stale。stale 结果 **不 reject、不抛 UI 错误**，
//     而是 resolve 一个带 stale=true 标记的结果，交由 UI 自行忽略。
//  3. dispose：terminate Worker 并 reject 所有 pending。
//
// 请求-回执用自增 id 配对（参考 FrameDecoderPool 的 pending Map + onMessage 模式），
// 但本任务是单 Worker + 串行 + latest-wins，不是并行池。
import type { RawDataDefs } from "../types";

// 一次 decode 请求的输入：identity 元组 + 解码所需的 schema/type/payload。
export interface RawDataDecodeRequest {
  generation: number;
  channel: string;
  seq: number;
  schemaId: string;
  messageType: string;
  payload: Uint8Array;
}

// decode 结果：携带与请求相同的 identity 元组 + text/lines/error，
// 以及 latest-wins 判定得到的 stale 标记（旧结果 stale=true，UI 应忽略）。
export interface RawDataDecodeResult {
  generation: number;
  channel: string;
  seq: number;
  text: string;
  lines: string[];
  error?: string;
  stale?: boolean;
}

// Worker → 主线程回包（与 rawDataWorker.ts 的 DecodeResponse 对齐）。
interface WorkerDecodeResponse {
  type: "decoded";
  id: number;
  generation: number;
  channel: string;
  seq: number;
  text: string;
  lines: string[];
  error?: string;
}

interface Pending {
  req: RawDataDecodeRequest;
  resolve: (r: RawDataDecodeResult) => void;
  reject: (e: Error) => void;
}

// Worker 工厂：默认用 vite 原生语法创建真 Worker；测试可注入 fake Worker。
type WorkerFactory = () => Worker;

const defaultFactory: WorkerFactory = () =>
  new Worker(new URL("./rawDataWorker.ts", import.meta.url), { type: "module" });

export class RawDataDecoder {
  private worker: Worker;
  private pending = new Map<number, Pending>();
  private nextId = 1;
  private disposed = false;
  // 已注册 definitions 前，decode 直接回结构化错误（definitions 必须先注册）。
  private hasDefs = false;
  // 当前 epoch（generation）：换源时前进，旧回包据此判 stale。
  private generation = 0;
  // 每通道已发起的最新 seq：回包若非该通道最新 seq，则判 stale。
  private latestSeqByChannel = new Map<string, number>();

  constructor(factory: WorkerFactory = defaultFactory) {
    this.worker = factory();
    this.worker.onmessage = (ev: MessageEvent<WorkerDecodeResponse>) =>
      this.onMessage(ev.data);
    // onerror：单 Worker 崩溃时拒绝所有 pending，避免静默悬挂。
    this.worker.onerror = () => this.rejectAll(new Error("RawData Worker 错误"));
  }

  // 注册最新 definitions：转发给 Worker（串行处理，先于后续 decode 生效）。
  setDefinitions(defs: RawDataDefs): void {
    if (this.disposed) return;
    this.hasDefs = true;
    this.worker.postMessage({ type: "defs", defs });
  }

  // 换源：推进 epoch。此后 generation 更小的回包一律 stale。
  setGeneration(generation: number): void {
    this.generation = generation;
  }

  // 提交一条 decode 请求。返回 Promise，成功/ stale 都 resolve；仅 dispose 时 reject。
  decode(req: RawDataDecodeRequest): Promise<RawDataDecodeResult> {
    if (this.disposed) {
      return Promise.reject(new Error("RawDataDecoder 已销毁"));
    }
    // definitions 必须先注册：未注册直接回结构化错误，不打扰 Worker。
    if (!this.hasDefs) {
      return Promise.resolve({
        generation: req.generation,
        channel: req.channel,
        seq: req.seq,
        text: "",
        lines: [],
        error: "RawData definitions 尚未注册（需先 setDefinitions）",
      });
    }

    // 记录本请求发起时的 epoch 与该通道最新 seq，用于回包时的 latest-wins 判定。
    if (req.generation > this.generation) this.generation = req.generation;
    this.latestSeqByChannel.set(req.channel, req.seq);

    const id = this.nextId++;
    // buffer 所有权关键：复制 payload，只 transfer 副本 buffer，绝不转移 BigDataStore
    // 持有的原 buffer（原 payload 因此不被 detach）。
    const copy = req.payload.slice();

    return new Promise<RawDataDecodeResult>((resolve, reject) => {
      this.pending.set(id, { req, resolve, reject });
      this.worker.postMessage(
        {
          type: "decode",
          id,
          generation: req.generation,
          channel: req.channel,
          seq: req.seq,
          schemaId: req.schemaId,
          messageType: req.messageType,
          payload: copy,
        },
        [copy.buffer],
      );
    });
  }

  private onMessage(resp: WorkerDecodeResponse): void {
    const p = this.pending.get(resp.id);
    if (!p) return;
    this.pending.delete(resp.id);

    // latest-wins：generation 已推进、或该通道已有更新 seq，则本结果 stale。
    const latestSeq = this.latestSeqByChannel.get(resp.channel);
    const stale =
      resp.generation < this.generation ||
      (latestSeq !== undefined && resp.seq < latestSeq);

    p.resolve({
      generation: resp.generation,
      channel: resp.channel,
      seq: resp.seq,
      text: resp.text,
      lines: resp.lines,
      error: resp.error,
      stale: stale || undefined,
    });
  }

  private rejectAll(err: Error): void {
    for (const p of this.pending.values()) p.reject(err);
    this.pending.clear();
  }

  dispose(): void {
    if (this.disposed) return;
    this.disposed = true;
    this.worker.terminate();
    this.rejectAll(new Error("RawDataDecoder 已销毁"));
  }
}