// WebSocket 传输实现(前后端分离部署形态)。实现 Transport 接口，
// 下行封包解析复用 messageCodec.parseServerMessage(与 FFI 路径同一契约)。
//
// 兼容说明：历史上此文件即 FrameStream，现拆分为 WsTransport(实现)+ 契约模块。
// 为减少上层改动，保留 FrameStream 作为 WsTransport 的导出别名。

import { parseServerMessage, type ServerMessageHandlers } from "./transport/messageCodec";
import type { ControlMsg, Transport, TransportHandlers } from "./transport/transport";

// 兼容再导出：现有 import 位置(threeEngine 等)无需改动。
export {
  FRAME_MESSAGE_TYPE,
  STREAM_INFO_MESSAGE_TYPE,
  FILE_LIST_MESSAGE_TYPE,
  ERROR_MESSAGE_TYPE,
  UPLOAD_STATUS_MESSAGE_TYPE,
  CHART_DEFS_MESSAGE_TYPE,
  LAYER_DEFS_MESSAGE_TYPE,
  IMAGE_DEFS_MESSAGE_TYPE,
  PREFETCH_FRAME_MESSAGE_TYPE,
} from "./transport/messageCodec";
export type { McapFileInfo, StreamInfo, UploadState } from "./transport/messageCodec";
export type { ControlMsg } from "./transport/transport";
// 历史类型名：等价于 TransportHandlers(下行回调 + 生命周期)。
export type FrameStreamHandlers = TransportHandlers;

const UPLOAD_CHUNK_BYTES = 1024 * 1024;

function websocketUrl(url: string): string {
  if (/^wss?:\/\//i.test(url)) return url;
  const scheme = window.location.protocol === "https:" ? "wss:" : "ws:";
  return new URL(url, `${scheme}//${window.location.host}`).toString();
}

export class WsTransport implements Transport {
  private ws: WebSocket | null = null;
  private generation = 0;

  constructor(private url: string, private handlers: TransportHandlers = {}) {}

  connect(): void {
    this.close();
    const ws = new WebSocket(websocketUrl(this.url));
    ws.binaryType = "arraybuffer";
    ws.onopen = () => this.handlers.onOpen?.();
    ws.onclose = (event) => {
      if (this.ws === ws) this.ws = null;
      this.handlers.onClose?.(event);
    };
    ws.onerror = (event) => this.handlers.onError?.(event);
    ws.onmessage = (event) => this.handleMessage(event.data);
    this.ws = ws;
  }

  listFiles(): void {
    this.send({ type: "listFiles" });
  }

  open(fileName: string, source: "local" | "s3" | "auto" = "local"): void {
    this.send({ type: "open", source, fileName });
  }

  // 统一打开入口：只传文件名，服务端在本地缓存目录命中时用本地 reader，否则按 record 名查 S3。
  openByName(fileName: string): void {
    this.send({ type: "open", source: "auto", fileName });
  }

  openRecord(record: string): void {
    this.send({ type: "open", source: "s3", record });
  }

  async upload(file: File, onProgress: (ratio: number) => void): Promise<void> {
    if (!this.ws || this.ws.readyState !== WebSocket.OPEN) throw new Error("数据服务尚未连接");
    this.send({ type: "uploadBegin", fileName: file.name, sizeBytes: file.size });
    for (let offset = 0; offset < file.size; offset += UPLOAD_CHUNK_BYTES) {
      while (this.ws.bufferedAmount > 4 * UPLOAD_CHUNK_BYTES) {
        await new Promise((resolve) => setTimeout(resolve, 20));
      }
      this.ws.send(await file.slice(offset, offset + UPLOAD_CHUNK_BYTES).arrayBuffer());
      onProgress(Math.min(1, (offset + UPLOAD_CHUNK_BYTES) / file.size));
    }
    while (this.ws.bufferedAmount > 0) await new Promise((resolve) => setTimeout(resolve, 20));
    this.send({ type: "uploadComplete" });
  }

  cancelUpload(): void {
    this.send({ type: "uploadCancel"});
  }

  seek(timeSec: number): void {
    this.generation += 1;
    this.send({ type: "seek", timeSec, generation: this.generation });
  }

  currentGeneration(): number {
    return this.generation;
  }

  subscribeImage(channel: string, enabled: boolean): void {
    this.send({ type: "subscribeImage", channel, enabled });
  }

  subscribePointCloud(channel: string, enabled: boolean): void {
    this.send({ type: "subscribePointCloud", channel, enabled });
  }

  subscribeRawData(channel: string, enabled: boolean): void {
    this.send({ type: "subscribeRawData", channel, enabled });
  }

  // 上报当前 playhead，驱动后端大数据预解码窗口移动。带当前代次供后端过滤过期请求
  //（seek 已自增 generation，随后立即上报即可让后端围绕新位置预解码大数据）。
  reportPlayhead(timeSec: number): void {
    this.send({ type: "playhead", timeSec, generation: this.generation });
  }

  send(msg: ControlMsg): void {
    if (!this.ws || this.ws.readyState !== WebSocket.OPEN) return;
    this.ws.send(JSON.stringify(msg));
  }

  isOpen(): boolean {
    return this.ws?.readyState === WebSocket.OPEN;
  }

  bufferedAmount(): number {
    return this.ws?.bufferedAmount ?? 0;
  }

  close(): void {
    if (this.ws) {
      this.ws.onopen = null;
      this.ws.onclose = null;
      this.ws.onerror = null;
      this.ws.onmessage = null;
      this.ws.close();
    }
    this.ws = null;
  }

  private handleMessage(data: unknown): void {
    if (!(data instanceof ArrayBuffer)) {
      this.handlers.onError?.(new Error("WebSocket 仅接受二进制消息"));
      return;
    }
    // 下行解析复用共享契约模块(与 FFI 路径同一实现)；onGeneration 同步本地代次镜像。
    parseServerMessage(data, {
      ...(this.handlers as ServerMessageHandlers),
      onGeneration: (generation) => {
        this.generation = generation;
      },
    });
  }
}

// 兼容别名：现有代码以 `new FrameStream(url, handlers)` 构造，等价于 WsTransport。
export const FrameStream = WsTransport;
export type FrameStream = WsTransport;