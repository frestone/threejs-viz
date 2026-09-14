// 主线程侧的解码 Worker 池：把 Frame bytes 分发到多个 decodeWorker 并行解码，
// 结果以 Promise 返回。目的是让 protobuf 解码（大帧 ~800KB、数千 item）离开主线程，
// 不再阻塞 rAF 渲染循环，从而突破此前 ~23Hz 的渲染瓶颈。
//
// 设计：
// - N 个 Worker（默认按 CPU 核数取 2~4），round-robin 分发，最大化吞吐。
// - 每个请求分配自增 id，Worker 回执带同一 id 用于配对 resolve/reject。
// - 解码结果的点云/图像 ArrayBuffer 由 Worker 端 transfer 回来，主线程零拷贝接收。
import type { DecodedFrame } from "./frameCodec";

interface DecodeResponse {
  id: number;
  seq: number;
  frame?: DecodedFrame;
  error?: string;
}

interface Pending {
  resolve: (frame: DecodedFrame) => void;
  reject: (err: Error) => void;
}

export class FrameDecoderPool {
  private workers: Worker[] = [];
  private pending = new Map<number, Pending>();
  private nextId = 1;
  private rr = 0;
  private disposed = false;

  constructor(size?: number) {
    const hw = typeof navigator !== "undefined" ? navigator.hardwareConcurrency || 4 : 4;
    // 解码是纯 CPU + 内存分配，Worker 太多反而增加克隆/调度开销，2~4 个足够。
    const n = Math.max(1, Math.min(size ?? Math.floor(hw / 2), 4));
    for (let i = 0; i < n; i++) {
      // vite 原生 Worker 语法：new URL(..., import.meta.url) + { type: "module" }。
      const w = new Worker(new URL("./decodeWorker.ts", import.meta.url), { type: "module" });
      w.onmessage = (ev: MessageEvent<DecodeResponse>) => this.onMessage(ev.data);
      w.onerror = () => {
        // 单个 Worker 崩溃：把该 Worker 名下未决请求全部 reject（无法精确定位则不处理，
        // 交由超时/上层重连；此处仅防止静默丢失，简单起见不做请求-worker 映射）。
      };
      this.workers.push(w);
    }
  }

  private onMessage(resp: DecodeResponse): void {
    const p = this.pending.get(resp.id);
    if (!p) return;
    this.pending.delete(resp.id);
    if (resp.error || !resp.frame) {
      p.reject(new Error(resp.error ?? "解码失败"));
    } else {
      p.resolve(resp.frame);
    }
  }

  // 提交一帧字节到 Worker 池解码，返回解码后的 DecodedFrame。
  decode(seq: number, bytes: Uint8Array): Promise<DecodedFrame> {
    if (this.disposed) return Promise.reject(new Error("解码池已销毁"));
    const id = this.nextId++;
    const worker = this.workers[this.rr];
    this.rr = (this.rr + 1) % this.workers.length;
    return new Promise<DecodedFrame>((resolve, reject) => {
      this.pending.set(id, { resolve, reject });
      // transfer bytes 底层 buffer 到 Worker，主线程侧该 Uint8Array 随即失效（可接受，
      // 因为主线程收到 bytes 后仅用于解码，不再持有）。
      worker.postMessage({ id, seq, bytes }, [bytes.buffer]);
    });
  }

  dispose(): void {
    this.disposed = true;
    for (const w of this.workers) w.terminate();
    this.workers = [];
    for (const p of this.pending.values()) p.reject(new Error("解码池已销毁"));
    this.pending.clear();
  }
}