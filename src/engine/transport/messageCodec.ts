// 服务端下行消息的"字节封包契约"单一来源。
//
// 【双部署契约一致性 · ADR-08】WebSocket 与 Tauri FFI 两条传输路径复用完全相同的
// 字节封包格式，因此下行消息的解析逻辑必须集中在此处，被 WsTransport 与 FfiTransport
// 共同调用，杜绝两路径各自实现导致的协议漂移。
//
// 封包格式（与后端 server.cpp / ffi C ABI 完全一致）：
//   - 帧类消息(type 1 / 9)：  [type:u8][seq:u64 LE][Frame proto bytes...]
//   - JSON 类消息(type 2~8)：  [type:u8][UTF-8 JSON...]

import type {
  ChartDef,
  ImageChannelDef,
  LayerDef,
  RawDataChannelDef,
  RawDataDefs,
  RawDataSchemaDef,
} from "../../types";

export const FRAME_MESSAGE_TYPE = 1;
export const STREAM_INFO_MESSAGE_TYPE = 2;
export const FILE_LIST_MESSAGE_TYPE = 3;
export const ERROR_MESSAGE_TYPE = 4;
export const UPLOAD_STATUS_MESSAGE_TYPE = 5;
export const CHART_DEFS_MESSAGE_TYPE = 6;
export const LAYER_DEFS_MESSAGE_TYPE = 7;
export const IMAGE_DEFS_MESSAGE_TYPE = 8;
// 全速预取帧：封包格式与 FRAME_MESSAGE_TYPE 完全相同(header+seq+Frame proto)，
// 语义为后台全量落盘用的非实时帧(后端遇背压不丢、重试补齐)。解析路径与普通帧一致。
export const PREFETCH_FRAME_MESSAGE_TYPE = 9;
// 静态地图帧：封包格式与 FRAME_MESSAGE_TYPE 完全相同(header+seq+Frame proto)，
// 会话建立时后端单独发一次。轻图静态地图数万点，若逐帧塞入会导致全量缓存膨胀
// (实测 750 帧 ~1GB)；此通道让前端单独常驻渲染、不进逐帧播放缓存。
export const STATIC_MAP_MESSAGE_TYPE = 10;
// 大数据独立流：图像/RawData 不进常规 Frame，围绕 playhead 实时下发，前端只持当前帧。
// 封包: [type:u8=11][gen:u32 LE][tSec:f64 LE][kind:u8 0=image 1=raw 2=thumbnail][seq:u32 LE][channelLen:u16 LE][channel][payload]
export const BIGDATA_MESSAGE_TYPE = 11;
// RawData Definitions(ChannelDefs)：封包 [type:u8=12][UTF-8 JSON]，与 type 2~8 JSON 类同构。
// 语义为一次性下发的 RawData 通道/schema 快照(换源时重发)。本处仅做严格解析+分派，
// 不 decode base64、不编译 protobuf descriptor(那是后续任务)。外层无 generation(代次由 type 11 帧头承担)。
export const RAW_DATA_DEFS_MESSAGE_TYPE = 12;
export const FRAME_HEADER_BYTES = 9;

export interface BigDataFrame {
  channel: string;
  tSec: number;
  gen: number;
  kind: "image" | "raw" | "thumbnail";
  // 原始消息 header.seq(ROS 风格 Header field1 uint32)。图像用于面板诊断显示；raw 为 0。
  seq: number;
  payload: Uint8Array;
}

export interface StreamInfo {
  durationSec: number;
  frameCount: number;
  generation: number;
  // 数据包运行模式：highprec=高精，lightmap=轻图（由服务端 resolveDataMode 判定）。
  dataMode?: "highprec" | "lightmap";
}

export interface McapFileInfo {
  name: string;
  sizeBytes: number;
}

export type UploadState = "ready" | "processing" | "complete";

// 下行消息处理回调集合：WsTransport / FfiTransport 均把收到的字节交给 parseServerMessage，
// 由后者按 type 分派到对应回调。传输层自身只负责"取到字节"，不参与协议解析。
export interface ServerMessageHandlers {
  onFrame?: (seq: bigint, bytes: Uint8Array) => void;
  onPrefetchFrame?: (seq: bigint, bytes: Uint8Array) => void;
  // 静态地图帧：解码后单独常驻渲染，不注入逐帧播放缓存。bytes 为 Frame proto(仅含 layers)。
  onStaticMap?: (seq: bigint, bytes: Uint8Array) => void;
  onInfo?: (info: StreamInfo) => void;
  onFileList?: (files: McapFileInfo[]) => void;
  onUploadStatus?: (state: UploadState) => void;
  onChartDefs?: (defs: ChartDef[]) => void;
  onLayerDefs?: (defs: LayerDef[]) => void;
  onImageDefs?: (defs: ImageChannelDef[]) => void;
  // RawData 通道/schema 定义快照(type 12)：严格解析后的 typed 结构，供动态 registry 消费。
  onRawDataDefs?: (defs: RawDataDefs) => void;
  onError?: (error: Error | Event) => void;
  // 大数据独立流帧：图像 JPEG 字节或 RawData 原始序列化字节，前端只持当前帧。
  onBigData?: (frame: BigDataFrame) => void;
  // 解析出 STREAM_INFO 时回传其 generation，供传输层更新本地代次镜像。
  onGeneration?: (generation: number) => void;
}

// 解析一条服务端下行二进制消息并分派到 handlers。WS 与 FFI 复用同一实现，
// 保证两路径协议解析零漂移。data 为完整封包(含 1 字节 type 头)。
export function parseServerMessage(data: ArrayBuffer, handlers: ServerMessageHandlers): void {
  if (data.byteLength < 1) {
    handlers.onError?.(new Error("收到空下行消息"));
    return;
  }

  const view = new DataView(data);
  const messageType = view.getUint8(0);

  if (
    messageType === FRAME_MESSAGE_TYPE ||
    messageType === PREFETCH_FRAME_MESSAGE_TYPE ||
    messageType === STATIC_MAP_MESSAGE_TYPE
  ) {
    if (data.byteLength < FRAME_HEADER_BYTES) {
      handlers.onError?.(new Error("Frame 消息头不足 9 字节"));
      return;
    }
    const seq = view.getBigUint64(1, true);
    const bytes = new Uint8Array(data, FRAME_HEADER_BYTES);
    if (messageType === PREFETCH_FRAME_MESSAGE_TYPE) {
      // 未单独提供预取回调时回退到 onFrame：两者消费路径同为"解码→注入缓存"。
      (handlers.onPrefetchFrame ?? handlers.onFrame)?.(seq, bytes);
    } else if (messageType === STATIC_MAP_MESSAGE_TYPE) {
      // 静态地图帧：只解码常驻渲染，绝不注入逐帧缓存。未接回调时静默丢弃(不回退 onFrame)。
      handlers.onStaticMap?.(seq, bytes);
    } else {
      handlers.onFrame?.(seq, bytes);
    }
    return;
  }

  if (messageType === BIGDATA_MESSAGE_TYPE) {
    const BIGDATA_MIN = 1 + 4 + 8 + 1 + 4 + 2;
    if (data.byteLength < BIGDATA_MIN) {
      handlers.onError?.(new Error("BigData 消息头不足"));
      return;
    }
    let o = 1;
    const gen = view.getUint32(o, true); o += 4;
    const tSec = view.getFloat64(o, true); o += 8;
    const kindByte = view.getUint8(o); o += 1;
    const seq = view.getUint32(o, true); o += 4;
    const chanLen = view.getUint16(o, true); o += 2;
    if (data.byteLength < BIGDATA_MIN + chanLen) {
      handlers.onError?.(new Error("BigData channel 越界"));
      return;
    }
    const channel = new TextDecoder().decode(new Uint8Array(data, o, chanLen)); o += chanLen;
    const payload = new Uint8Array(data, o);
    handlers.onBigData?.({
      channel,
      tSec,
      gen,
      kind: kindByte === 1 ? "raw" : kindByte === 2 ? "thumbnail" : "image",
      seq,
      payload,
    });
    return;
  }

  // 其余 type 2~8 均为 [type:u8][UTF-8 JSON] 结构。
  let json: string;
  try {
    json = new TextDecoder().decode(new Uint8Array(data, 1));
  } catch (error) {
    handlers.onError?.(error instanceof Error ? error : new Error(String(error)));
    return;
  }

  try {
    switch (messageType) {
      case STREAM_INFO_MESSAGE_TYPE: {
        const parsed = JSON.parse(json) as StreamInfo;
        if (!Number.isFinite(parsed.durationSec) || !Number.isInteger(parsed.frameCount)) {
          throw new Error("流元数据字段非法");
        }
        handlers.onGeneration?.(parsed.generation);
        handlers.onInfo?.(parsed);
        return;
      }
      case FILE_LIST_MESSAGE_TYPE: {
        const parsed = JSON.parse(json) as { files: McapFileInfo[] };
        if (!Array.isArray(parsed.files)) throw new Error("MCAP 文件列表格式非法");
        handlers.onFileList?.(parsed.files);
        return;
      }
      case ERROR_MESSAGE_TYPE: {
        const parsed = JSON.parse(json) as { message?: string };
        handlers.onError?.(new Error(parsed.message ?? "服务端错误"));
        return;
      }
      case CHART_DEFS_MESSAGE_TYPE: {
        const parsed = JSON.parse(json) as { charts?: ChartDef[] };
        if (!Array.isArray(parsed.charts))throw new Error("图表配置格式非法");
        handlers.onChartDefs?.(parsed.charts);
        return;
      }
      case LAYER_DEFS_MESSAGE_TYPE: {
        const parsed = JSON.parse(json) as { layers?: LayerDef[] };
        if (!Array.isArray(parsed.layers)) throw new Error("图层样式配置格式非法");
        handlers.onLayerDefs?.(parsed.layers);
        return;
      }
      case IMAGE_DEFS_MESSAGE_TYPE: {
        const parsed = JSON.parse(json) as { images?: ImageChannelDef[] };
        if (!Array.isArray(parsed.images)) throw new Error("相机图像通道配置格式非法");
        handlers.onImageDefs?.(parsed.images);
        return;
      }
      case RAW_DATA_DEFS_MESSAGE_TYPE: {
        // 严格字段级校验：任何不符即 throw，被下方 catch 转 onError，且不触发 onRawDataDefs。
        const defs = parseRawDataDefs(JSON.parse(json));
        handlers.onRawDataDefs?.(defs);
        return;
      }
      case UPLOAD_STATUS_MESSAGE_TYPE: {
        const parsed = JSON.parse(json) as { state?: UploadState };
        if (!parsed.state) throw new Error("上传状态字段缺失");
        handlers.onUploadStatus?.(parsed.state);
        return;
      }
      default:
        handlers.onError?.(new Error(`未知下行消息类型: ${messageType}`));
    }
  } catch (error) {
    handlers.onError?.(error instanceof Error ? error : new Error(String(error)));
  }
}

// ── type 12 RawData Definitions 严格解析 ──────────────────────────────────
// 仅解析+校验+分派，不 decode base64、不编译 protobuf descriptor(Task 5/6 负责)。
// 任何字段不符即 throw Error，由调用方 catch 转 onError；容忍未知多余字段。

function isString(v: unknown): v is string {
  return typeof v === "string";
}

// 合法 base64 校验：允许标准 base64 字符集(含 = 填充)，长度为 4 的倍数，仅末尾至多 2 个 =。
// 空串视为合法(0 字节)。用正则而非 atob——保持在 Node/浏览器双环境下行为一致、无副作用。
const BASE64_RE = /^(?:[A-Za-z0-9+/]{4})*(?:[A-Za-z0-9+/]{2}==|[A-Za-z0-9+/]{3}=)?$/;
function isValidBase64(v: string): boolean {
  return BASE64_RE.test(v);
}

// 校验单个 RawData 通道定义。available 分支字段互斥：
// available=true 必带 messageType+schemaId(string)；available=false 时 unavailableReason 若存在须为 string。
function parseRawDataChannel(raw: unknown, index: number): RawDataChannelDef {
  if (typeof raw !== "object" || raw === null) {
    throw new Error(`RawData 通道[${index}]非对象`);
  }
  const o = raw as Record<string, unknown>;
  if (!isString(o.id)) throw new Error(`RawData 通道[${index}].id 必须为 string`);
  if (!isString(o.topic)) throw new Error(`RawData 通道[${index}].topic 必须为 string`);
  if (!isString(o.label)) throw new Error(`RawData 通道[${index}].label 必须为 string`);
  if (typeof o.available !== "boolean") {
    throw new Error(`RawData 通道[${index}].available 必须为 boolean`);
  }
  const def: RawDataChannelDef = {
    id: o.id,
    topic: o.topic,
    label: o.label,
    available: o.available,
  };
  if (o.available) {
    if (!isString(o.messageType)) {
      throw new Error(`RawData 通道[${index}] available=true 时 messageType 必须为 string`);
    }
    if (!isString(o.schemaId)) {
      throw new Error(`RawData 通道[${index}] available=true 时 schemaId 必须为 string`);
    }
    def.messageType = o.messageType;
    def.schemaId = o.schemaId; // 保持 string，不转 number
  } else {
    // available=false：unavailableReason 可容忍缺省，但若存在必须为 string。
    if (o.unavailableReason !== undefined) {
      if (!isString(o.unavailableReason)) {
        throw new Error(`RawData 通道[${index}].unavailableReason 若存在必须为 string`);
      }
      def.unavailableReason = o.unavailableReason;
    }
  }
  return def;
}

// 校验单个 schema：id(string)/encoding==="protobuf"/dataBase64(合法 base64 string，原样透传)。
function parseRawDataSchema(raw: unknown, index: number): RawDataSchemaDef {
  if (typeof raw !== "object" || raw === null) {
    throw new Error(`RawData schema[${index}]非对象`);
  }
  const o = raw as Record<string, unknown>;
  if (!isString(o.id)) throw new Error(`RawData schema[${index}].id 必须为 string`);
  if (o.encoding !== "protobuf") {
    throw new Error(`RawData schema[${index}].encoding 必须为 "protobuf"`);
  }
  if (!isString(o.dataBase64)) {
    throw new Error(`RawData schema[${index}].dataBase64 必须为 string`);
  }
  if (!isValidBase64(o.dataBase64)) {
    throw new Error(`RawData schema[${index}].dataBase64 非法 base64`);
  }
  return { id: o.id, encoding: "protobuf", dataBase64: o.dataBase64 };
}

// 校验 type 12 外层结构：rawData/schemas 必须是数组(外层无 generation)。容忍未知多余键。
function parseRawDataDefs(raw: unknown): RawDataDefs {
  if (typeof raw !== "object" || raw === null) {
    throw new Error("RawData 定义快照非对象");
  }
  const o = raw as Record<string, unknown>;
  if (!Array.isArray(o.rawData)) throw new Error("RawData 定义 rawData 必须为数组");
  if (!Array.isArray(o.schemas)) throw new Error("RawData 定义 schemas 必须为数组");
  const rawData = o.rawData.map((c, i) => parseRawDataChannel(c, i));
  const schemas = o.schemas.map((s, i) => parseRawDataSchema(s, i));
  return { rawData, schemas };
}