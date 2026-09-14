// Frame 解码层：用 protobufjs 运行时加载 /frame.proto，将 WebSocket 推流的
// Frame 二进制解码为前端友好的 TS 结构。免 protoc 生成步骤——proto 文件放在
// public/frame.proto，运行期 fetch 加载。
//
// 坐标系约定（与 viz-core/proto/frame.proto 一致）：右手系 x 前 / y 左 / z 上。
// 转换到 Three.js（y 上）由 threeEngine 负责，本层只做纯数据解码。
import protobuf from "protobufjs";
import type { ChartData, ChartSeriesData, ChartSeriesKind } from "../types";

// --- 解码后前端结构 ---------------------------------------------------------

export type DecodedGeometryKind = "point" | "linestrip" | "box" | "polygon" | "text" | "arrow" | "planning_trajectory";

export interface Vec3 {
  x: number;
  y: number;
  z: number;
}

export interface DecodedGeometryItem {
  position?: Vec3;
  // packed xyz：每 3 个 float 为一点 [x0,y0,z0, x1,y1,z1, ...]。
  // 用 Float32Array 而非 Vec3[]：proto 里一点仅 12B，展开成 {x,y,z} JS 对象在 V8 约 64B（膨胀~5×），
  // 是播放缓存内存爆炸的主因；packed 存储对齐 xplayer-ts 的 decode 语义，且可零拷贝喂给 BufferGeometry。
  points: Float32Array;
  size?: Vec3;
  heading: number;
  text: string;
  id: number;
  type: number;
  score: number;
}

export interface DecodedLayer {
  kind: DecodedGeometryKind;
  items: DecodedGeometryItem[];
}

// 解码后的一帧图像（相机画面）。data 为编码字节，供前端转 Blob/ImageBitmap 显示。
export interface DecodedImage {
  format: string;
  width: number;
  height: number;
  data: Uint8Array;
  t: number;
}

// 解码后的一帧点云（对齐 PointCloud proto）。xyz 为 packed float，每 3 个为一点。
// rgb（可选，0..1）与 intensity（可选）按点对齐，供渲染层按 colorMode 上色。
export interface DecodedPointCloud {
  xyz: Float32Array;      // [x0,y0,z0, x1,y1,z1, ...] 右手系 x前/y左/z上
  rgb?: Float32Array;     // [r,g,b, ...] 每点 3 个，0..1，可空
  intensity?: Float32Array; // 每点 1 个，可空
  t: number;
  frameId: string;
  encoding: string;       // 压缩标识，空=raw
}

// 解码后的一帧原始数据（对齐 RawData proto）。data 为原始字节流，语义由 format 决定。
export interface DecodedRawData {
  topic: string;
  format: string;
  data: Uint8Array;
  t: number;
  frameId: string;
  encoding: string;
  seq: number;
}

export interface DecodedFrame {
  t: number;
  layers: Record<string, DecodedLayer>;
  egoAnchor: Vec3;
  egoYaw: number;
  egoValid: boolean;
  charts: ChartData[];
  // 按相机名索引的图像（key = 相机通道名）。
  images: Record<string, DecodedImage>;
  // 按点云图层 id 索引的点云（key = decoder pointClouds[].id / Frame.point_clouds key）。
  pointClouds: Record<string, DecodedPointCloud>;
  // 按 rawData 图层 id 索引的原始数据（key = decoder rawData[].id / Frame.raw_data key）。
  rawData: Record<string, DecodedRawData>;
}

// proto 枚举下标 → 前端字符串。
const GEOMETRY_KINDS: DecodedGeometryKind[] = ["point", "linestrip", "box", "polygon", "text", "arrow", "planning_trajectory"];
const CHART_SERIES_KINDS: ChartSeriesKind[] = ["line", "scatter", "band_upper", "band_lower"];

// --- protobufjs 类型缓存 ----------------------------------------------------

let frameType: protobuf.Type | null = null;
let loadPromise: Promise<protobuf.Type> | null = null;

// perception 障碍物类型配置（id -> 名称 + 颜色），单一来源为
// public/obstacle_colors.json（由 ROVER5.0 perception_type.proto 转换而来）。
// 运行期 fetch 加载，新增/调整障碍物类型只需改该 JSON，无需改前端代码。
interface ObstacleTypeEntry {
  id: number;
  name: string;
  color: string; // #RRGGBB
}
let perceptionTypeNames: Record<number, string> = {};
let perceptionTypeColors: Record<number, string> = {};

//障碍物 type 数值 → 名称（如 1 -> "CAR"）。未加载或未知值回退 "UNKNOWN"。
export function perceptionTypeName(type: number): string {
  return perceptionTypeNames[type] ?? "UNKNOWN";
}

// 障碍物 type 数值 → 颜色十六进制串（如 "#66BFFF"）。未配置返回 null，由渲染层兜底。
export function perceptionTypeColorHex(type: number): string | null {
  return perceptionTypeColors[type] ?? null;
}

// 加载并缓存 Frame message 类型。首次调用触发 fetch("/frame.proto")；
// 同时加载 obstacle_colors.json 派生障碍物类型名/颜色映射。
export async function loadFrameCodec(protoUrl = "/frame.proto"): Promise<void> {
  if (frameType) return;
  if (!loadPromise) {
    // keepCase: true 保持 proto 字段原名（snake_case，如 ego_anchor/ego_valid/x_label），
    // 与本文件下方按 snake_case 的读取一致。若用默认的 protobuf.load（keepCase=false），
    // 字段会被转成 camelCase，导致 obj.ego_valid 等恒为 undefined → egoValid 恒 false
    // → 相机跟随失效（旋转中心退回世界原点）。
    loadPromise = fetch(protoUrl)
      .then((r) => r.text())
      .then((text) => protobuf.parse(text, { keepCase: true }).root.lookupType("viz.Frame"));
    // 并行加载障碍物类型配置，失败不阻断主 codec。
    fetch("/obstacle_colors.json")
      .then((r) => r.json())
      .then((cfg: { types?: ObstacleTypeEntry[] }) => {
        const names: Record<number, string> = {};
        const colors: Record<number, string> = {};
        for (const t of cfg.types ?? []) {
          names[t.id] = t.name;
          colors[t.id] = t.color;
        }
        perceptionTypeNames = names;
        perceptionTypeColors = colors;
      })
      .catch(() => {
        /* 配置缺失时保持空表，name 回退 UNKNOWN、color 回退渲染层兜底 */
      });
  }
  frameType = await loadPromise;
}

function toVec3(v: unknown): Vec3 {
  const o = (v ?? {}) as { x?: number; y?: number; z?: number };
  return { x: o.x ?? 0, y: o.y ?? 0, z: o.z ?? 0 };
}

// 把 proto 解出的 points 数组（[{x,y,z}, ...]）打包成 packed Float32Array（每 3 个一点），
// 避免在 DecodedFrame 中长期驻留大量 {x,y,z} JS 对象。空/缺失返回长度 0 的数组。
function packPoints(raw: unknown[] | undefined): Float32Array {
  if (!raw || raw.length === 0) return new Float32Array(0);
  const out = new Float32Array(raw.length * 3);
  for (let i = 0; i < raw.length; i++) {
    const p = (raw[i] ?? {}) as { x?: number; y?: number; z?: number };
   out[i * 3] = p.x ?? 0;
    out[i * 3 + 1] = p.y ?? 0;
    out[i * 3 + 2] = p.z ?? 0;
  }
  return out;
}

// packed points 的点数（Float32Array 长度 / 3）。
export function pointCount(points: Float32Array): number {
  return points.length / 3;
}

// 把 packed points 第 i 个点读进复用的 out 对象（零分配遍历）。返回 out 便于链式使用。
export function pointAt(points: Float32Array, i: number, out: Vec3): Vec3 {
  const b = i * 3;
  out.x = points[b];
  out.y = points[b + 1];
  out.z = points[b + 2];
  return out;
}

// --- 图表映射（对齐 ChartData/ChartSeries proto）---------------------------

function toCharts(raw: Record<string, unknown> | undefined): ChartData[] {
  if (!raw) return [];
  const out: ChartData[] = [];
  for (const [id, value] of Object.entries(raw)) {
    const c = value as {
      title?: string;
      x_label?: string;
      y_label?: string;
      series?: unknown[];
    };
    const series: ChartSeriesData[] = (c.series ?? []).map((s) => {
      const item = s as {
        name?: string;
        x?: number[];
        y?: number[];
        kind?: number;
        color?: string;
      };
      return {
        name: item.name ?? "",
        kind: CHART_SERIES_KINDS[item.kind ?? 0] ?? "line",
        color: item.color ?? "",
        x: item.x ?? [],
        y: item.y ?? [],
      };
    });
    out.push({
      id,
      title: c.title ?? id,
      xLabel: c.x_label ?? "",
      yLabel: c.y_label ?? "",
      series,
    });
  }
  return out;
}

// --- 图像映射（对齐 Image proto）------------------------------------------

function toImages(raw: Record<string, unknown> | undefined): Record<string, DecodedImage> {
  const out: Record<string, DecodedImage> = {};
  if (!raw) return out;
  for (const [name, value] of Object.entries(raw)) {
    const im = value as {
      format?: string;
      width?: number;
      height?: number;
      data?: Uint8Array | number[];
      t?: number;
    };
    const data =
      im.data instanceof Uint8Array ? im.data : new Uint8Array(im.data ?? []);
    out[name] = {
      format: im.format ?? "",
      width: im.width ?? 0,
      height: im.height ?? 0,
      data,
      t: im.t ?? 0,
    };
  }
  return out;
}

// --- 点云映射（对齐 PointCloud proto）--------------------------------------

function toFloat32(v: Uint8Array | number[] | Float32Array | undefined): Float32Array | undefined {
  if (!v || (v as ArrayLike<number>).length === 0) return undefined;
  if (v instanceof Float32Array) return v;
  return Float32Array.from(v as ArrayLike<number>);
}

function toPointClouds(raw: Record<string, unknown> | undefined): Record<string, DecodedPointCloud> {
  const out: Record<string, DecodedPointCloud> = {};
  if (!raw) return out;
  for (const [id, value] of Object.entries(raw)) {
    const pc = value as {
      xyz?: number[] | Float32Array;
      rgb?: number[] | Float32Array;
      intensity?: number[] | Float32Array;
      t?: number;
      frame_id?: string;
      encoding?: string;
    };
    out[id] = {
      xyz: toFloat32(pc.xyz) ?? new Float32Array(0),
      rgb: toFloat32(pc.rgb),
      intensity: toFloat32(pc.intensity),
      t: pc.t ?? 0,
      frameId: pc.frame_id ?? "",
      encoding: pc.encoding ?? "",
    };
  }
  return out;
}

// --- rawData 映射（对齐 RawData proto）------------------------------------

function toUint8(v: Uint8Array | number[] | undefined): Uint8Array {
  if (!v || (v as ArrayLike<number>).length === 0) return new Uint8Array(0);
  if (v instanceof Uint8Array) return v;
  return Uint8Array.from(v as ArrayLike<number>);
}

function toRawData(raw: Record<string, unknown> | undefined): Record<string, DecodedRawData> {
  const out: Record<string, DecodedRawData> = {};
  if (!raw) return out;
  for (const [id, value] of Object.entries(raw)) {
    const rd = value as {
      topic?: string;
      format?: string;
      data?: Uint8Array | number[];
      t?: number;
      frame_id?: string;
      encoding?: string;
      seq?: number;
    };
    out[id] = {
      topic: rd.topic ?? "",
      format: rd.format ?? "",
      data: toUint8(rd.data),
      t: rd.t ?? 0,
      frameId: rd.frame_id ?? "",
      encoding: rd.encoding ?? "",
      seq: rd.seq ?? 0,
    };
  }
  return out;
}

// --- 主解码入口 -------------------------------------------------------------

// 将 Frame 序列化 bytes 解码为前端结构。必须先 await loadFrameCodec()。
export function decodeFrame(bytes: Uint8Array): DecodedFrame {
  if (!frameType) throw new Error("Frame codec 未初始化，请先调用 loadFrameCodec()");

  // decode → toObject(defaults) 得到普通 JS 对象；枚举以数值返回。
  const message = frameType.decode(bytes);
  const obj = frameType.toObject(message, {
    defaults: true,
    arrays: true,
    objects: true,
    enums: Number,
    longs: Number,
  }) as {
    t?: number;
    layers?: Record<string, unknown>;
    ego_anchor?: unknown;
    ego_yaw?: number;
    ego_valid?: boolean;
    charts?: Record<string, unknown>;
    images?: Record<string, unknown>;
    point_clouds?: Record<string, unknown>;
    raw_data?: Record<string, unknown>;
  };

  const layers: Record<string, DecodedLayer> = {};
  for (const [name, value] of Object.entries(obj.layers ?? {})) {
    const layer = value as { kind?: number; items?: unknown[] };
    const items: DecodedGeometryItem[] = (layer.items ?? []).map((it) => {
      const item = it as {
        position?: unknown;
        points?: unknown[];
        size?: unknown;
        heading?: number;
        text?: string;
        id?: number;
        type?: number;
        score?: number;
      };
      return {
        position: item.position ? toVec3(item.position) : undefined,
        points: packPoints(item.points),
        size: item.size ? toVec3(item.size) : undefined,
        heading: item.heading ?? 0,
        text: item.text ?? "",
        id: item.id ?? 0,
        type: item.type ?? 0,
        score: item.score ?? 0,
      };
    });
    layers[name] = { kind: GEOMETRY_KINDS[layer.kind ?? 0] ?? "point", items };
  }

  return {
    t: obj.t ?? 0,
    layers,
    egoAnchor: toVec3(obj.ego_anchor),
    egoYaw: obj.ego_yaw ?? 0,
    egoValid: obj.ego_valid ?? false,
    charts: toCharts(obj.charts),
    images: toImages(obj.images),
    pointClouds: toPointClouds(obj.point_clouds),
    rawData: toRawData(obj.raw_data),
  };
}

// 收集一帧中所有可 Transfer 的 ArrayBuffer（点云 xyz/rgb/intensity 与相机图像 data），
// 供 Web Worker 用 postMessage 的 transfer 列表零拷贝回传主线程，避免大数组结构化克隆。
export function collectTransferables(frame: DecodedFrame): ArrayBuffer[] {
  const out: ArrayBuffer[] = [];
  for (const pc of Object.values(frame.pointClouds)) {
    if (pc.xyz && pc.xyz.byteLength > 0) out.push(pc.xyz.buffer as ArrayBuffer);
    if (pc.rgb && pc.rgb.byteLength > 0) out.push(pc.rgb.buffer as ArrayBuffer);
    if (pc.intensity && pc.intensity.byteLength > 0) out.push(pc.intensity.buffer as ArrayBuffer);
  }
  for (const img of Object.values(frame.images)) {
    if (img.data && img.data.byteLength > 0) out.push(img.data.buffer as ArrayBuffer);
  }
  for (const layer of Object.values(frame.layers)) {
    for (const item of layer.items) {
      if (item.points.byteLength > 0) out.push(item.points.buffer as ArrayBuffer);
    }
  }
  return out;
}

// 估算一帧在 JS 堆中的实际驻留字节，供播放缓存按“总内存”而非“帧数”做上限背压。
// 帧数上限无法感知单帧胖瘦（一帧几十万点云 vs 空帧差几个数量级），按字节数才能把内存真正夹住。
// 点云/图像用 TypedArray 的精确 byteLength；layers 的 item/point 是 JS 小对象，
// 按 V8 经验值粗估（每个 {x,y,z} 对象含对象头约 ~64B，每个 item 约 ~96B）。宁可高估，安全优先。
const BYTES_PER_VEC3_OBJ = 64;   // {x,y,z} 一个对象在 V8 中的近似占用（含对象头/指针）
const BYTES_PER_ITEM = 96;       // DecodedGeometryItem 一个对象的基础占用（不含 points）
export function estimateFrameBytes(frame: DecodedFrame): number {
  let bytes = 256; // 帧对象本身 + Record 容器基础开销
  for (const pc of Object.values(frame.pointClouds)) {
    bytes += pc.xyz?.byteLength ?? 0;
    bytes += pc.rgb?.byteLength ?? 0;
    bytes += pc.intensity?.byteLength ?? 0;
  }
  for (const img of Object.values(frame.images)) {
    bytes += img.data?.byteLength ?? 0;
  }
  for (const layer of Object.values(frame.layers)) {
    for (const item of layer.items) {
      bytes += BYTES_PER_ITEM;
      if (item.position) bytes += BYTES_PER_VEC3_OBJ;
      if (item.size) bytes += BYTES_PER_VEC3_OBJ;
      bytes += item.points.byteLength; // packed Float32Array：每点 12B（不再是 64B JS 对象）
    }
  }
  return bytes;
}