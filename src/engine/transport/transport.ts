// 前端传输抽象层：解耦"播放引擎(threeEngine)"与"具体传输(WebSocket / Tauri FFI)"。
//
// 【双部署 · ADR-08】前后端分离形态走 WsTransport；桌面(Tauri)形态走 FfiTransport
// (进程内直连后端动态库)。两者实现同一 Transport 接口，threeEngine 只依赖该接口，
// 不感知底层是 WebSocket 还是 FFI，且上行控制/下行封包复用同一 frame.proto 契约。

import type { ServerMessageHandlers } from "./messageCodec";

// 上行控制消息：前端 → 后端。WS 以 JSON 文本发送，FFI 逐字段映射到 C ABI 调用，
// 但字段结构完全一致(单一契约来源)，与后端 server.cpp control 分派逐一对应。
export type ControlMsg =
  | { type: "listFiles" }
  | { type: "open"; source: "local" | "s3" | "auto"; fileName: string }
  | { type: "open"; source: "s3"; record: string }
  | { type: "uploadBegin"; fileName: string; sizeBytes: number }
  | { type: "uploadComplete" }
  | { type: "uploadCancel" }
  | { type: "seek"; timeSec: number; generation: number }
  | { type: "setPaused"; paused: boolean }
  | { type: "setSpeed"; speed: number }
  // 全速预取触发：加载完成后请求后端另起预取通道，后台全量落盘(type 9 预取帧)。幂等。
  | { type: "startPrefetch" }
  | { type: "setLayerVisible"; layerId: string; visible: boolean }
  | { type: "subscribeImage"; channel: string; enabled: boolean }
  | { type: "subscribePointCloud"; channel: string; enabled: boolean }
  | { type: "subscribeRawData"; channel: string; enabled: boolean }
  // 上报当前 playhead + 代次，驱动后端大数据预解码窗口移动。
  | { type: "playhead"; timeSec: number; generation: number };

// 传输层连接生命周期回调(下行消息处理回调见 ServerMessageHandlers)。
export interface TransportLifecycle {
  onOpen?: () => void;
  onClose?: (event?: unknown) => void;
}

export type TransportHandlers = ServerMessageHandlers & TransportLifecycle;

// 前端传输接口：WsTransport / FfiTransport 均实现之。方法面对齐现有 FrameStream 公开面，
// threeEngine 以此为唯一依赖，保证切换传输实现零改动上层。
export interface Transport {
  // 建立连接(WS: open socket；FFI: 打开会话动态库句柄)。
  connect(): void;

  // === 上行控制 ===
  listFiles(): void;
  open(fileName: string, source?: "local" | "s3" | "auto"): void;
  openByName(fileName: string): void;
  openRecord(record: string): void;
  seek(timeSec: number): void;
  subscribeImage(channel: string, enabled: boolean): void;
  subscribePointCloud(channel: string, enabled: boolean): void;
  subscribeRawData(channel: string, enabled: boolean): void;
  // 通用控制发送(setPaused / setSpeed / startPrefetch / setLayerVisible 等)。
  send(msg: ControlMsg): void;

  // 大文件分块上传(WS: 二进制分片；FFI: 走本地文件路径直读，实现内部差异化)。
  upload(file: File, onProgress: (ratio: number) => void): Promise<void>;
  cancelUpload(): void;

  // === 状态查询 ===
  currentGeneration(): number;
  isOpen(): boolean;
  // 当前下行/上行缓冲积压字节(WS: ws.bufferedAmount；FFI: 队列估算)，供背压判断。
  bufferedAmount(): number;

  // 关闭连接并释放资源。
  close(): void;
}