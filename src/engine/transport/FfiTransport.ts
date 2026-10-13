// Tauri 桌面进程内 FFI 传输。Rust 仅转发 C++ 产生的完整字节封包，
// 下行解析与 WebSocket 共用 messageCodec，避免双路径协议漂移。

import { parseServerMessage, type ServerMessageHandlers } from "./messageCodec";
import type { ControlMsg, Transport, TransportHandlers } from "./transport";
import { ConnectionAttempt } from "./connectionAttempt";

type Invoke = <T>(command: string, args?: Record<string, unknown>) => Promise<T>;

export class FfiTransport implements Transport {
  private generation = 0;
  private connected = false;
  private connecting = false;
  private invokeFn: Invoke | null = null;
  private attempts = new ConnectionAttempt();
  // 【连接前命令队列】引擎构造（stream.connect()）到 ffi_connect 完成之间存在窗口期：
  // 此时 createEngine 已返回，上层可能立刻下发 open / subscribeImage / setPaused 等命令。
  // 早期实现在未连接时直接 return 丢弃，导致这些命令永久丢失——表现为"文件打不开/
  // 相机图像永不出图"，且前端无任何报错。改为按序暂存，连接建立后补发。
  private pending: Array<{ command: string; args?: Record<string, unknown> }> = [];

  constructor(private handlers: TransportHandlers = {}) {}

  connect(): void {
    if (this.connected || this.connecting) return;
    this.connecting = true;
    const attempt = this.attempts.begin();
    void this.initialize(attempt);
  }

  private async initialize(attempt: number): Promise<void> {
    try {
      const { invoke, Channel } = await import("@tauri-apps/api/core");
      this.invokeFn = invoke as Invoke;
      if (!this.attempts.isCurrent(attempt)) return;
      // 下行大帧（~830KB/帧）经 Tauri Channel 二进制快速通道接收，避免 emit 把 Vec<u8>
      // 序列化成 JSON 整数数组压垮主线程。Channel<Vec<u8>> 前端 onmessage 收到 ArrayBuffer。
      const channel = new Channel<ArrayBuffer>();
      channel.onmessage = (payload) => {
        if (!this.attempts.isCurrent(attempt)) return;
        const buffer =
          payload instanceof ArrayBuffer ? payload : new Uint8Array(payload).buffer;
        // 预取帧(type=9)与 RawData 大数据帧(type=11 kind=1)在 C++ 侧累加在途字节水位做
        // 背压;前端消费后立即回落等量字节(整包长度 = C++ 侧 fetch_add 的 buf.size())。
        // 图像帧(type=11 kind=0)在 Desktop Original 模式下不进入该水位：它由后端
        // 32帧有界实时队列控制，前端只显示最新帧；误 ack 反而会抵消预取水位。
        if (buffer.byteLength >= 1) {
          const view = new DataView(buffer);
          const t = view.getUint8(0);
          const bigDataKind = t === 11 ? view.getUint8(13) : -1;
          if (t === 9 || (t === 11 && bigDataKind !== 0)) {
            this.call("ffi_ack_frame", { bytes: buffer.byteLength });
          }
        }
        parseServerMessage(buffer, {
          ...(this.handlers as ServerMessageHandlers),
          onGeneration: (generation) => {
            this.generation = generation;
          },
        });
      };
      // 必须先绑定 channel 再创建原生会话，否则会丢失会话初始化阶段下发的配置。
      await invoke("ffi_connect", { channel });
      if (!this.attempts.isCurrent(attempt)) {
        await invoke("ffi_close").catch(() => undefined);
        return;
      }
      this.connected = true;
      this.connecting = false;
      // 补发连接窗口期内被暂存的命令（顺序保持：先于 onOpen 下发的先执行）。
      this.flushPending();
      this.handlers.onOpen?.();
    } catch (error) {
      this.connecting = false;
      this.handlers.onError?.(error instanceof Error ? error : new Error(String(error)));
    }
  }

  listFiles(): void {
    // 桌面通过原生文件对话框直接选择绝对路径，不需要远端文件列表。
    this.handlers.onFileList?.([]);
  }

  open(fileName: string, _source: "local" | "s3" | "auto" = "local"): void {
    this.call("ffi_open", { source: fileName });
  }

  openByName(fileName: string): void {
    this.open(fileName);
  }

  openRecord(record: string): void {
    this.open(record);
  }

  seek(timeSec: number): void {
    this.generation += 1;
    this.call("ffi_seek", { timeSec, generation: this.generation });
  }

  subscribeImage(channel: string, enabled: boolean): void {
    this.call("ffi_subscribe_image", { channel, enabled });
  }

  subscribePointCloud(channel: string, enabled: boolean): void {
    this.call("ffi_subscribe_point_cloud", { channel, enabled });
  }

  subscribeRawData(channel: string, enabled: boolean): void {
    this.call("ffi_subscribe_raw_data", { channel, enabled });
  }

  send(msg: ControlMsg): void {
    switch (msg.type) {
      case "seek":
        this.call("ffi_seek", { timeSec: msg.timeSec, generation: msg.generation });
        return;
      case "playhead":
        // 桌面 FFI 必须转发 playhead 到后端 SetPlayhead，驱动 BigDataRun 前瞻预解码图像/RawData。
        // 缺此分支时暂停态勾选相机通道后 BigDataRun 无 playhead 驱动，图像帧永不下发、面板不弹。
        this.call("ffi_set_playhead", {
          timeSec: msg.timeSec,
          generation: msg.generation,
        });
        return;
      case "setPaused":
        this.call("ffi_set_paused", { paused: msg.paused });
        return;
      case "setSpeed":
        this.call("ffi_set_speed", { speed: msg.speed });
        return;
      case "startPrefetch":
        this.call("ffi_start_prefetch");
        return;
      case "subscribeImage":
        this.subscribeImage(msg.channel, msg.enabled);
        return;
      case "subscribePointCloud":
        this.subscribePointCloud(msg.channel, msg.enabled);
        return;
      case "subscribeRawData":
        this.subscribeRawData(msg.channel, msg.enabled);
        return;
      case "open":
        this.open("record" in msg ? msg.record : msg.fileName, msg.source);
        return;
      case "listFiles":
        this.listFiles();
        return;
      // 图层显隐仅影响前端场景；上传控制在桌面绝对路径模式下不适用。
      case "setLayerVisible":
      case "uploadBegin":
      case "uploadComplete":
      case "uploadCancel":
        return;
    }
  }

  async upload(_file: File, _onProgress: (ratio: number) => void): Promise<void> {
    throw new Error("桌面模式请使用原生文件选择器打开本地 MCAP 文件");
  }

  cancelUpload(): void {}

  currentGeneration(): number {
    return this.generation;
  }

  isOpen(): boolean {
    return this.connected;
  }

  bufferedAmount(): number {
    return 0;
  }

  close(): void {
    const wasConnected = this.connected || this.connecting;
    this.attempts.cancel();
    this.connected = false;
    this.connecting = false;
    this.pending = [];
    if (this.invokeFn) void this.invokeFn("ffi_close").catch(() => undefined);
    if (wasConnected) this.handlers.onClose?.();
  }

  private call(command: string, args?: Record<string, unknown>): void {
    // 连接尚未建立：暂存而非丢弃，待 ffi_connect 完成后按序补发（见 pending 字段说明）。
    if (!this.connected || !this.invokeFn) {
      this.pending.push({ command, args });
      return;
    }
    void this.invokeFn(command, args).catch((error) => {
      this.handlers.onError?.(error instanceof Error ? error : new Error(String(error)));
    });
  }

  // 连接建立后按序补发暂存命令。invokeFn 在 initialize 早期即已赋值，此处必然可用。
  private flushPending(): void {
    if (!this.invokeFn || this.pending.length === 0) return;
    const queued = this.pending;
    this.pending = [];
    for (const { command, args } of queued) {
      void this.invokeFn(command, args).catch((error) => {
        this.handlers.onError?.(error instanceof Error ? error : new Error(String(error)));
      });
    }
  }
}
