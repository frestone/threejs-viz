// Frame 解码 Web Worker：把 protobuf 解码 + 结构化展开从主线程剥离，避免大帧
// （~800KB、数千 item）在 WS onmessage 回调里同步解码阻塞主线程 rAF，导致渲染
// 掉到 ~23Hz。主线程只负责把收到的 bytes 转发给本 Worker，解码结果经 postMessage
// （transfer 点云/图像 ArrayBuffer 零拷贝）回传后 insertFrame 入缓存。
//
// 复用 frameCodec 的 loadFrameCodec/decodeFrame：Worker 与主线程各持一份 protobufjs
// 类型缓存（互不共享内存，但同源可各自 fetch /frame.proto，代价仅一次）。
import { loadFrameCodec, decodeFrame, collectTransferables } from "./frameCodec";

// 主线程 → Worker 的请求：id 用于回执配对，bytes 为 Frame 序列化字节。
interface DecodeRequest {
  id: number;
  seq: number;
  bytes: Uint8Array;
}

// Worker → 主线程的响应：成功带 frame，失败带 error。
interface DecodeResponse {
  id: number;
  seq: number;
  frame?: unknown;
  error?: string;
}

// proto codec 就绪前先把请求排队，就绪后统一冲刷，保证不丢首批帧。
let ready = false;
const pending: DecodeRequest[] = [];

loadFrameCodec()
  .then(() => {
    ready = true;
    for (const req of pending) handle(req);
    pending.length = 0;
  })
  .catch((e) => {
    // codec 加载失败：把已排队请求全部回错误，避免主线程无限等待。
    const msg = e instanceof Error ? e.message : "codec 加载失败";
    for (const req of pending) {
      const resp: DecodeResponse = { id: req.id, seq: req.seq, error: msg };
      (self as unknown as Worker).postMessage(resp);
    }
    pending.length = 0;
  });

function handle(req: DecodeRequest): void {
  try {
    const frame = decodeFrame(req.bytes);
    const transfer = collectTransferables(frame);
    const resp: DecodeResponse = { id: req.id, seq: req.seq, frame };
    (self as unknown as Worker).postMessage(resp, transfer);
  } catch (e) {
    const resp: DecodeResponse = {
      id: req.id,
      seq: req.seq,
      error: e instanceof Error ? e.message : "解码失败",
    };
    (self as unknown as Worker).postMessage(resp);
  }
}

self.onmessage = (ev: MessageEvent<DecodeRequest>) => {
  const req = ev.data;
  if (ready) handle(req);
  else pending.push(req);
};