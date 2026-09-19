// Three.js 渲染引擎：替代原 WASM + Filament 渲染核心。
// 职责：连接后端 WebSocket（FrameStream）→ decodeFrame → 按图层几何语义渲染 →
// 相机跟随 ego。播放控制逻辑与原 createStreamEngine 一致。
//
// 坐标系：后端 proto 为右手系 x 前 / y 左 / z 上；Three.js 默认 y 上。
// 处理方式：把整个 world group 绕 X 轴旋转 -90°，使 proto 的 +z(上) 对齐 three 的
// +y(上)，proto 的 +y(左) 对齐 three 的 -z。这样几何顶点可直接用 (x, y, z) 填入。
import * as THREE from "three";
import { OrbitControls } from "three/examples/jsm/controls/OrbitControls.js";
import type { ChartData, EngineApi } from "../types";
import { FrameStream, type McapFileInfo } from "./FrameStream";
import { FfiTransport } from "./transport/FfiTransport";
import type { Transport, TransportHandlers } from "./transport/transport";
import type { TransportMode } from "./transport/transportMode";
import { loadFrameCodec, perceptionTypeName, perceptionTypeColorHex, estimateFrameBytes, pointCount, type DecodedFrame, type DecodedLayer, type DecodedPointCloud, type Vec3 } from "./frameCodec";
import { FrameDecoderPool } from "./frameDecoder";
import type { ChartDef, LayerDef, ImageChannelDef, CameraMode, DataMode, RawDataDefs } from "../types";
import { RawDataDecoder } from "./rawDataDecoder";
import { createRawDataWiring } from "./rawDataWiring";
import type { PanelState } from "../rawDataPanelState";
import { isDebugEnabled } from "./perfLog";
export interface ThreeEngineOptions {
  canvas: HTMLCanvasElement;
  wsUrl?: string;
  transportMode?: TransportMode;
  sourceFile?: string;
  onFileList?: (files: McapFileInfo[]) => void;
  onChartDefs?: (defs: ChartDef[]) => void;
  // 服务端连接建立即下发的图层样式定义（源自 decoder.json 的 layers）。
  // 透传给上层用于动态构建图层显隐复选框（轻图新增图层无需前端硬编码）。
  onLayerDefs?: (defs: LayerDef[]) => void;
  // 服务端连接建立即下发的相机图像通道定义（源自 decoder*.json 的 imageChannels）。
  // 透传给上层用于在图像组动态构建相机复选框；默认不勾选、不订阅（零成本）。
  onImageDefs?: (defs: ImageChannelDef[]) => void;
  onStatus?: (status: string, error?: boolean) => void;
  // 已订阅相机图像的每帧回调：channel=相机通道名，jpeg=JPEG 字节。
  // 与渲染时钟对齐——在实际渲染某帧时才回调该帧携带的图像，避免画面/图像错位。
  onImage?: (channel: string, jpeg: Uint8Array, width: number, height: number) => void;
  // 大数据（图像/RawData）独立流：某 channel 的当前帧被替换时回调，通知上层重新拉取。
  // 与常规缓存分离——大数据不全量缓存，仅按 playhead 实时持有当前帧。
  onBigDataUpdate?: (channel: string, kind: "image" | "raw" | "thumbnail") => void;
  // RawData 通道定义（type-12 ChannelDefs）：连接建立即下发，供上层动态构建
  // RawData 面板通道复选框；默认不勾选、不订阅（零成本，与 image 一致）。
  onRawDataDefs?: (defs: RawDataDefs) => void;
  // RawData 解码面板 state 变化回调：定义/选择/解码结果变更时通知上层重绘面板。
  onRawDataPanelUpdate?: (state: PanelState) => void;
}

// 大数据（图像/RawData）当前帧持有者。与常规全量缓存分离：每个 channel 只保留
// “当前 playhead 对应的一帧”，代次校验丢弃过期帧，替换时 revoke 旧 blobUrl，
// 内存复杂度 O(可见通道数) 而非 O(帧数)。
export interface BigDataEntry {
  tSec: number;
  gen: number;
  kind: "image" | "raw";
  // 原始消息 header.seq(图像诊断序号；raw 为 0)。面板叠加显示。
  seq: number;
  payload: Uint8Array;
  blobUrl?: string;
}

// 大数据帧输入形状（与 messageCodec.BigDataFrame 结构一致，避免跨模块类型耦合）。
interface BigDataFrameInput {
  channel: string;
  tSec: number;
  gen: number;
  kind: "image" | "raw" | "thumbnail";
  seq: number;
  payload: Uint8Array;
}

// 创建大数据持有者。getGen 返回当前有效代次——seek 会自增代次，
// 落后代次的帧（网络在途的旧数据）在 handle 时被直接丢弃，避免残留。
export function createBigDataStore(getGen: () => number) {
  const map = new Map<string, BigDataEntry>();
  return {
    // 收到一帧大数据：代次过期则丢弃；否则 revoke 旧 blobUrl 后替换当前帧。
    handle(frame: BigDataFrameInput): void {
      if (frame.gen !== getGen()) return;
      // 缩略图不进当前帧持有者（走 thumbnailStore 全量缓存）；此处防御性丢弃。
      if (frame.kind === "thumbnail") return;
      const prev = map.get(frame.channel);
      if (prev?.blobUrl) {
        try { URL.revokeObjectURL(prev.blobUrl); } catch { /* ignore */ }
      }
      map.set(frame.channel, {
        tSec: frame.tSec,
        gen: frame.gen,
        kind: frame.kind,
        seq: frame.seq,
        payload: frame.payload,
      });
    },
    get(channel: string): BigDataEntry | undefined {
      return map.get(channel);
    },
    // 清空全部通道（如切换数据源）：revoke 所有 blobUrl。
    clear(): void {
      for (const e of map.values()) {
        if (e.blobUrl) { try { URL.revokeObjectURL(e.blobUrl); } catch { /* ignore */ } }
      }
      map.clear();
    },
    size(): number {
      return map.size;
    },
  };
}

// 缩略图全量缓存（kind=thumbnail）。与 bigDataStore(仅当前帧)分离——缩略图后台懒生成、
// 全量常驻供进度条拖动时按 previewTime 就近查显示(低清跟手)。
// 结构: channel -> 按 tSec 升序的 {tSec, blobUrl, seq} 列表 + seq 去重集合。
// LRU 兜底: 全局按插入序维护 URL 总数上限,超限 revoke 最旧。
// 【容量取值】缩略图默认 480x270，实际尺寸按通道配置；目标是“拖动全程缩略图不丢”，故 capacity
// 需覆盖最大总帧数(全通道×全时长抽稀后)。6 路 × ~2000 帧(10min@25fps 抽稀余量)= 12000,
// 内存代价 12000 × ~15KB ≈ 180MB,几百 MB 级完全可接受(远小于原图全量缓存的数 GB)。
// 提高到 12000 消除拖回已看位置时因 LRU 驱逐导致的"缩略图丢失",实现真·全量常驻。
export interface ThumbEntry { tSec: number; blobUrl: string; seq: number; }
export function createThumbnailStore(getGen: () => number, capacity = 12000) {
  interface Chan { list: ThumbEntry[]; seqSet: Set<number>; }
  const map = new Map<string, Chan>();
  // 全局 LRU 插入序: 记录 (channel, seq) 便于超限时定位并 revoke 最旧条目。
  const lru: Array<{ channel: string; seq: number }> = [];
  let total = 0;

  function revokeOldest() {
    while (total > capacity && lru.length > 0) {
      const oldest = lru.shift();
      if (!oldest) break;
      const ch = map.get(oldest.channel);
      if (!ch) continue;
      const i = ch.list.findIndex((e) => e.seq === oldest.seq);
      if (i < 0) continue;
      try { URL.revokeObjectURL(ch.list[i].blobUrl); } catch { /* ignore */ }
      ch.list.splice(i, 1);
      ch.seqSet.delete(oldest.seq);
      total--;
    }
  }

  return {
    // 收到一帧缩略图: 高于当前代次的在途帧丢弃; 同包 seek 后旧代次缓存仍然有效,
    // 因此只拒绝更新代次; seq 已存在去重; 否则建 blobUrl 按 tSec 升序插入。
    handle(frame: BigDataFrameInput): void {
      if (frame.gen > getGen()) return;
      let ch = map.get(frame.channel);
      if (!ch) { ch = { list: [], seqSet: new Set() }; map.set(frame.channel, ch); }
      if (ch.seqSet.has(frame.seq)) return;  // 去重(后端已去重,双保险)
      const p = frame.payload;
      const buf = p.buffer.slice(p.byteOffset, p.byteOffset + p.byteLength) as ArrayBuffer;
      const blob = new Blob([buf], { type: "image/jpeg" });
      const blobUrl = URL.createObjectURL(blob);
      // 二分定位插入点保持 tSec 升序(多数为追加,少数回头补扫需插中间)。
      let lo = 0, hi = ch.list.length;
      while (lo < hi) {
        const mid = (lo + hi) >> 1;
        if (ch.list[mid].tSec < frame.tSec) lo = mid + 1; else hi = mid;
      }
      ch.list.splice(lo, 0, { tSec: frame.tSec, blobUrl, seq: frame.seq });
      ch.seqSet.add(frame.seq);
      lru.push({ channel: frame.channel, seq: frame.seq });
      total++;
      if (total > capacity) revokeOldest();
    },
    // 按时间就近查该通道完整缩略图条目，确保预览图与源图像序号同源。
    findNearestEntry(channel: string, tSec: number): ThumbEntry | null {
      const ch = map.get(channel);
      if (!ch || ch.list.length === 0) return null;
      let lo = 0, hi = ch.list.length - 1;
      while (lo < hi) {
        const mid = (lo + hi) >> 1;
        if (ch.list[mid].tSec < tSec) lo = mid + 1; else hi = mid;
      }
      // lo 为首个 >= tSec 的下标; 比较其与前一个,取时间距离更近者。
      let best = lo;
      if (lo > 0 && Math.abs(ch.list[lo - 1].tSec - tSec) <= Math.abs(ch.list[lo].tSec - tSec)) {
        best = lo - 1;
      }
      return ch.list[best];
    },
    // 清空全部通道(换源/重开流): revoke 所有 blobUrl。
    clear(): void {
      for (const ch of map.values()) {
        for (const e of ch.list) {
          try { URL.revokeObjectURL(e.blobUrl); } catch { /* ignore */ }
        }
      }
      map.clear();
      lru.length = 0;
      total = 0;
    },
    size(): number { return total; },
  };
}

// 障碍物按 type 上色。颜色与类型名的单一来源是 public/obstacle_colors.json
// （由 perception_type.proto 转换而来）：优先用配置中的颜色，未配置的类型
// 按名称稳定 hash 生成一个可区分的 HSL 色，故新增类型无需改代码。
function colorForType(type: number): THREE.Color {
  const hex = perceptionTypeColorHex(type);
  if (hex) return new THREE.Color(hex);
  const name = perceptionTypeName(type);
  let h = 0;
  for (let i = 0; i < name.length; i++) h = (h * 31 + name.charCodeAt(i)) >>> 0;
  return new THREE.Color().setHSL((h % 360) / 360, 0.65, 0.6);
}

// 图层样式（颜色 + z 抬升）。运行时由后端下发的 LAYER_DEFS 填充；
// 在样式尚未到达（或某图层无配置）时用内建默认兜底。
interface LayerStyle {
  color: THREE.Color;
  colorByType: boolean;
  height: number; // z 抬升，避免与地面 z-fighting
  width: number;  // 线宽 / ribbon(轨迹带) 宽度，单位米
}

// 内建默认样式，仅作兜底。图层配色的唯一来源是后端下发的 LAYER_DEFS
// （源自 decoder.json 顶层 layers 或后端 SceneConfig::defaults）。
const DEFAULT_LAYER_STYLES: Record<string, LayerStyle> = {
  localization: { color: new THREE.Color(0.2, 0.9, 1.0), colorByType: false, height: 0, width: 1 },
  trajectory: { color: new THREE.Color(0.3, 1.0, 0.5), colorByType: false, height: 0.1, width: 1 },
  path: { color: new THREE.Color(1.0, 0.85, 0.3), colorByType: false, height: 0.05, width: 1 },
  perception: { color: new THREE.Color(1.0, 0.42, 0.42), colorByType: true, height: 0, width: 1 },
};

// 把后端下发的 LayerDef.style 转为渲染用 LayerStyle。渲染按 Frame 图层 key
// 查样式，故以 def.source 为索引键。
function styleFromDef(def: LayerDef): LayerStyle {
  const c = def.style.color;
  return {
    color: new THREE.Color(c.r, c.g, c.b),
    colorByType: def.style.colorByType,
    height: def.style.height,
    width: def.style.width,
  };
}

function v3(p: Vec3, dz = 0): THREE.Vector3 {
  return new THREE.Vector3(p.x, p.y, p.z + dz);
}

// 从 packed points（Float32Array，每 3 个一点）第 i 点建 THREE.Vector3（z 加偏移 dz）。
function v3p(points: Float32Array, i: number, dz = 0): THREE.Vector3 {
  const b = i * 3;
  return new THREE.Vector3(points[b], points[b + 1], points[b + 2] + dz);
}

// 计算朝向 box 的世界矩阵：以 center 为原点，绕 Z 轴旋转 yaw（弧度）。
function boxMatrix(center: Vec3, size: Vec3, yaw: number): THREE.Matrix4 {
  const m = new THREE.Matrix4();
  const q = new THREE.Quaternion().setFromEuler(new THREE.Euler(0, 0, yaw));
  // box 几何以底面 z=0、顶面 z=size.z：故中心抬到 size.z/2。
  const pos = new THREE.Vector3(center.x, center.y, center.z + size.z / 2);
  m.compose(pos, q, new THREE.Vector3(size.x, size.y, size.z));
  return m;
}

// 单个图层的可复用渲染容器：一个 group，按帧重建其子对象。
class LayerRenderer {
  readonly group = new THREE.Group();
  private disposables: (THREE.BufferGeometry | THREE.Material | THREE.Texture)[] = [];
  // 上一次 build 使用的几何+样式签名。若本帧签名相同（典型如静态轻图地图，
  // 每帧下发的几何完全一致）则跳过 clear()+重建，避免每帧重建数万点的巨大开销。
  private lastSig = "";

  constructor(readonly layerId: string) {}

  // 用几何结构（item 数、每 item 点数、首尾点）与样式派生一个轻量签名。
  // 不逐点比较，兼顾判别力与速度：静态图层帧间签名恒定，动态图层几乎必变。
  private static signature(layer: DecodedLayer, style: LayerStyle): string {
    let sig = layer.kind + "|" + layer.items.length +"|";
    sig += style.color.getHexString() + (style.colorByType ? "T" : "F") + style.height + "w" + style.width + "|";    for (const it of layer.items) {
      const n = pointCount(it.points);
      sig += it.type + ":" + n;
      if (n > 0) {
        const ax = it.points[0], ay = it.points[1];
        const bb = (n - 1) * 3;
        const bx = it.points[bb], by = it.points[bb + 1];
        sig += "(" + ax.toFixed(2) + "," + ay.toFixed(2) + ";" + bx.toFixed(2) + "," + by.toFixed(2) + ")";
      }
      // point/box 图层几何在 position/heading/size 上（非 points 数组）：ego 与
      // 障碍物随帧移动时 points 可能为空，须把位姿纳入签名，否则位置变了签名不变
      // 会被误判为静态而跳过重建，导致自车/障碍物“不动”。
      if (it.position) {
        sig += "@" + it.position.x.toFixed(2) + "," + it.position.y.toFixed(2) +
               "," + it.position.z.toFixed(2) + "^" + it.heading.toFixed(3);
      }
      if (it.size) {
        sig += "#" + it.size.x.toFixed(2) + "," + it.size.y.toFixed(2) + "," + it.size.z.toFixed(2);
      }
      sig += ";";
    }
    return sig;
  }

  // 清空上一帧子对象并释放 GPU 资源。
  clear(): void {
    for (const child of this.group.children) this.group.remove(child);
    while (this.group.children.length) this.group.remove(this.group.children[0]);
    for (const d of this.disposables) d.dispose();
    this.disposables = [];
  }

  private track<T extends THREE.BufferGeometry | THREE.Material | THREE.Texture>(x: T): T {
    this.disposables.push(x);
    return x;
  }

  // 依据图层几何类型重建。样式由 SceneRenderer 传入（后端下发或默认兜底）。
  build(layer: DecodedLayer, style: LayerStyle): void {
    // 数据+样式未变则复用上一帧几何（静态轻图地图每帧几何一致，无需重建）。
    const sig = LayerRenderer.signature(layer, style);
    if (sig === this.lastSig) return;
    this.lastSig = sig;
    this.clear();
    switch (layer.kind) {
      case "box":
        this.buildBoxes(layer, style);
        break;
      case "linestrip":
      case "polygon":
        this.buildPolylines(layer, style, layer.kind === "polygon");
        break;
      case "point":
        this.buildPoints(layer, style);
        break;
      case "arrow":
        this.buildArrows(layer, style);
        break;
      case "planning_trajectory":
        this.buildTrajectoryRibbon(layer, style);
        break;
      case "text":
        this.buildTexts(layer, style);
        break;
    }
  }

  private buildBoxes(layer: DecodedLayer, style: LayerStyle): void {
    // ── 规则 box(position+size) 合批缓冲 ──
    // 所有规则 box 的填充面合成单个 Mesh、棱线合成单个 LineSegments，
    // 使 draw call 与障碍物数解耦(原来每障碍物 3 个独立对象 → 现在 2 个合批对象 + N 个标签)。
    // colorByType 时用逐顶点颜色区分不同障碍类型。
    const faceVerts: number[] = [];
    const faceColors: number[] = [];
    const edgeVerts: number[] = [];
    const edgeColors: number[] = [];
    // 单位模板几何(边长 1、中心在原点),复用其顶点经 boxMatrix 变换后写入合批缓冲。
    const unitBox = new THREE.BoxGeometry(1, 1, 1);
    const unitEdges = new THREE.EdgesGeometry(unitBox);
    const boxPos = unitBox.getAttribute("position");
    const boxIdx = unitBox.getIndex();
    const edgePos = unitEdges.getAttribute("position");
    const tmpV = new THREE.Vector3();
    const pushTransformed = (
      attr: THREE.BufferAttribute | THREE.InterleavedBufferAttribute,
      i: number,
      mtx: THREE.Matrix4,
      out: number[]
    ) => {
      tmpV.set(attr.getX(i), attr.getY(i), attr.getZ(i)).applyMatrix4(mtx);
      out.push(tmpV.x, tmpV.y, tmpV.z);
    };

    for (const item of layer.items) {
      // polygon 障碍（如 POINT_CLOUD_CLUSTER 融合跟踪目标）：把底面闭合环沿 +z 拉伸
      // 成固定高度 1 的棱柱——底环 + 顶环 + 竖棱线，并加半透明侧面便于识别体积。
      if (item.points.length > 0) {
        const color = style.colorByType ? colorForType(item.type) : style.color;
        const EXTRUDE_H = 1; // 固定拉伸高度
        const np = pointCount(item.points);
        const base: THREE.Vector3[] = new Array(np);
        const top: THREE.Vector3[] = new Array(np);
        for (let i = 0; i < np; i++) {
          base[i] = v3p(item.points, i, style.height);
          top[i] = v3p(item.points, i, style.height + EXTRUDE_H);
        }
        // 底环 + 顶环（闭合）。
        for (const ring of [base, top]) {
          const rp = ring.slice();
          if (rp.length > 1) rp.push(rp[0].clone());
          const rgeom = this.track(new THREE.BufferGeometry().setFromPoints(rp));
          const rmat = this.track(new THREE.LineBasicMaterial({ color }));
          this.group.add(new THREE.Line(rgeom, rmat));
        }
        // 竖棱：连接每个底点与对应顶点。
        const pillars: THREE.Vector3[] = [];
        for (let i = 0; i < base.length; i++) {
          pillars.push(base[i].clone(), top[i].clone());
        }
        const pillarGeom = this.track(new THREE.BufferGeometry().setFromPoints(pillars));
        const pillarMat = this.track(new THREE.LineBasicMaterial({ color }));
        this.group.add(new THREE.LineSegments(pillarGeom, pillarMat));
        // 半透明侧面：相邻底/顶点构成的四边形（两三角）。
        if (base.length >= 2) {
          const verts: number[] = [];
          for (let i = 0; i < base.length; i++) {
            const j = (i + 1) % base.length;
            const b0 = base[i], b1 = base[j], t0 = top[i], t1 = top[j];
            verts.push(b0.x, b0.y, b0.z, b1.x, b1.y, b1.z, t1.x, t1.y, t1.z);
            verts.push(b0.x, b0.y, b0.z, t1.x, t1.y, t1.z, t0.x, t0.y, t0.z);
          }
          const sideGeom = this.track(new THREE.BufferGeometry());
          sideGeom.setAttribute("position", new THREE.Float32BufferAttribute(verts, 3));
          const sideMat = this.track(
            new THREE.MeshBasicMaterial({
              color,
              transparent: true,
              opacity: 0.18,
              depthWrite: false,
              side: THREE.DoubleSide,
            })
          );
          this.group.add(new THREE.Mesh(sideGeom, sideMat));
        }
        // 文字标签：#id TYPE，锚点取点集质心并抬到柱顶上方。
        const c = { x: 0, y: 0, z: 0 };
        for (let i = 0; i < np; i++) {
          const b = i * 3;
          c.x += item.points[b];
          c.y += item.points[b + 1];
          c.z = Math.max(c.z, item.points[b + 2]);
        }
        c.x /= np;
        c.y /= np;
        const text = item.text && item.text.length > 0
          ? item.text
          : `#${item.id ?? "?"} ${perceptionTypeName(item.type)}`;
        const label = makeTextSprite(text, color);
        label.position.copy(v3(c, style.height + EXTRUDE_H + 0.6));
        const labelMat = label.material as THREE.SpriteMaterial;
        this.track(labelMat);
        // 注意:不 track labelMat.map —— 纹理由 textSpriteCache 全局复用,不能被 clear() 销毁。
        this.group.add(label);
        continue;
      }
      if (!item.position || !item.size) continue;
      const color = style.colorByType ? colorForType(item.type) : style.color;
      const mtx = boxMatrix(item.position, item.size, item.heading);
      // 填充面:按模板 box 的索引三角展开成非索引顶点写入合批缓冲(便于逐顶点上色)。
      if (boxIdx) {
        for (let k = 0; k < boxIdx.count; k++) {
          pushTransformed(boxPos, boxIdx.getX(k), mtx, faceVerts);
          faceColors.push(color.r, color.g, color.b);
        }
      }
      // 棱线:模板 EdgesGeometry 顶点直接变换写入(已是 LineSegments 顶点对)。
      for (let k = 0; k < edgePos.count; k++) {
        pushTransformed(edgePos, k, mtx, edgeVerts);
        edgeColors.push(color.r, color.g, color.b);
      }
      // 障碍物标签：id + type，定位于 box 顶部。标签数与障碍物同阶,暂不合批。
      const label = makeTextSprite(`#${item.id ?? "?"} ${perceptionTypeName(item.type)}`, color);
      label.position.copy(v3(item.position, item.size.z + 0.6));
      const labelMat = label.material as THREE.SpriteMaterial;
      this.track(labelMat);
      // 注意:不 track labelMat.map —— 纹理由 textSpriteCache 全局复用,不能被 clear() 销毁。
      this.group.add(label);
    }

    // 模板几何用完即弃(未加入场景,不进 disposables)。
    unitBox.dispose();
    unitEdges.dispose();

    // ── 合批填充面 ──
    if (faceVerts.length > 0) {
      const g = this.track(new THREE.BufferGeometry());
      g.setAttribute("position", new THREE.Float32BufferAttribute(faceVerts, 3));
      g.setAttribute("color", new THREE.Float32BufferAttribute(faceColors, 3));
      const m = this.track(
        new THREE.MeshBasicMaterial({
          vertexColors: true,
          transparent: true,
          opacity: 0.25,
          depthWrite: false,
        })
      );
      this.group.add(new THREE.Mesh(g, m));
    }
    // ── 合批棱线 ──
    if (edgeVerts.length > 0) {
      const g = this.track(new THREE.BufferGeometry());
      g.setAttribute("position", new THREE.Float32BufferAttribute(edgeVerts, 3));
      g.setAttribute("color", new THREE.Float32BufferAttribute(edgeColors, 3));
      const m = this.track(new THREE.LineBasicMaterial({ vertexColors: true }));
      this.group.add(new THREE.LineSegments(g, m));
    }
  }

  // 规划轨迹带：把 points 折线沿其法向左右各扩展 width/2，构成连续三角带多边形。
  // 相比 1px 的 LineBasicMaterial（WebGL 下线宽恒为 1px，style.width=0.1 时几乎不可见），
  // 用真实几何宽度绘制，宽度由 style.width（米，JSON 配置，默认 1）决定。
  private buildTrajectoryRibbon(layer: DecodedLayer, style: LayerStyle): void {
    const halfW = (style.width > 0 ? style.width : 1) / 2;
    const h = style.height;
    const verts: number[] = [];
    for (const item of layer.items) {
      const p = item.points;
      const n = pointCount(p);
      if (n < 2) continue;
      // 逐段计算法向，生成两侧偏移点，再把相邻两截面组成两个三角形。
      let prevL: [number, number] | null = null;
      let prevR: [number, number] | null = null;
      for (let i = 0; i < n; i++) {
        // 该点方向：取前后相邻段的平均方向。
        const ai = Math.max(0, i - 1) * 3;
        const bi = Math.min(n - 1, i + 1) * 3;
        let dx = p[bi] - p[ai], dy = p[bi + 1] - p[ai + 1];
        const len = Math.hypot(dx, dy) || 1;
        dx /= len; dy /= len;
        // 法向 = 方向逆时针旋转 90°。
        const nx = -dy, ny = dx;
        const ci = i * 3;
        const cx = p[ci], cy = p[ci + 1];
        const l: [number, number] = [cx + nx * halfW, cy + ny * halfW];
        const r: [number, number] = [cx - nx * halfW, cy - ny * halfW];
        if (prevL && prevR) {
          // 两个三角形：prevL,prevR,l 与 prevR,r,l
          verts.push(prevL[0], prevL[1], h, prevR[0], prevR[1], h, l[0], l[1], h);
          verts.push(prevR[0], prevR[1], h, r[0], r[1], h, l[0], l[1], h);
        }
        prevL = l; prevR = r;
      }
    }
    if (verts.length === 0) return;
    const geom = this.track(new THREE.BufferGeometry());
    geom.setAttribute("position", new THREE.Float32BufferAttribute(verts, 3));
    const mat = this.track(
      new THREE.MeshBasicMaterial({
        color: style.color,
        transparent: true,
        opacity: 0.75,
        depthWrite: false,
        side: THREE.DoubleSide,
      })
    );
    this.group.add(new THREE.Mesh(geom, mat));
  }

  private buildPolylines(layer: DecodedLayer, style: LayerStyle, closed: boolean): void {
    // width（米，JSON 配置）足够大时用真实几何宽度的带状 Mesh 绘制折线：WebGL 下
    // LineBasicMaterial 的线宽恒为 1px，无法通过 style.width 加粗。阈值以下（细线/
    // 未特意配置宽度）仍走高效的 LineSegments 合批，避免地图大量细线退化为 Mesh。
    if (!closed && style.width > 0.05) {
      this.buildRibbonPolylines(layer, style);
      return;
    }
    const mat = this.track(new THREE.LineBasicMaterial({ color: style.color }));
    // polygon（closed=true）额外用半透明填充面渲染，便于识别区域（人行横道/禁停区等）。
    const fillMat = closed
      ? this.track(
          new THREE.MeshBasicMaterial({
            color: style.color,
            transparent: true,
            opacity:0.25,
            depthWrite: false,
            side: THREE.DoubleSide,
          })
        )
      : null;
    // 性能：把整层所有线段/填充面各合批为“一个” geometry 一次性绘制，避免每条线
    // 一个 THREE.Line 导致 draw call 随线数线性膨胀（轻图地图几百上千条线 →
    // 几百上千 draw call，是 renderer.render 把 rAF 拖到 ~20Hz 的主因）。
    // LineSegments 要求顶点成对（每段 a,b），故把折线拆成相邻点对。
    const lineVerts: number[] = [];
    const fillVerts: number[] = [];
    const h = style.height;
    for (const item of layer.items) {
      const p = item.points;
      const n = pointCount(p);
      if (n < 2) continue;
      const last = closed ? n : n - 1;
      for (let i = 0; i < last; i++) {
        const ai = i * 3;
        const bi = ((i + 1) % n) * 3;
        lineVerts.push(p[ai], p[ai + 1], h, p[bi], p[bi + 1], h);
      }
      // 填充面：扇形三角化（凸多边形足够；地图面元素一般近似凸/简单多边形）。
      if (fillMat && n >= 3) {
        const a0 = p[0], a1 = p[1];
        for (let i = 1; i + 1 < n; i++) {
          const bi = i * 3, ci = (i + 1) * 3;
          fillVerts.push(a0, a1, h, p[bi], p[bi + 1], h, p[ci], p[ci + 1], h);
        }
      }
    }
    if (lineVerts.length > 0) {
      const geom = this.track(new THREE.BufferGeometry());
      geom.setAttribute("position", new THREE.Float32BufferAttribute(lineVerts, 3));
      this.group.add(new THREE.LineSegments(geom, mat));
    }
    if (fillMat && fillVerts.length > 0) {
      const fillGeom = this.track(new THREE.BufferGeometry());
      fillGeom.setAttribute("position", new THREE.Float32BufferAttribute(fillVerts, 3));
      this.group.add(new THREE.Mesh(fillGeom, fillMat));
    }
  }

  // 用真实几何宽度绘制（未闭合）折线：沿每段法向左右各扩展 width/2 组成三角带，
  // 合批为单个 Mesh。宽度由 style.width（米，JSON 配置）决定，解决 LineBasicMaterial
  // 无法加粗（WebGL 线宽恒 1px）的问题。逻辑同 buildTrajectoryRibbon，但支持整层多条线合批。
  private buildRibbonPolylines(layer: DecodedLayer, style: LayerStyle): void {
    const halfW = style.width / 2;
    const h = style.height;
    const verts: number[] = [];
    for (const item of layer.items) {
      const p = item.points;
      const n = pointCount(p);
      if (n < 2) continue;
      let prevL: [number, number] | null = null;
      let prevR: [number, number] | null = null;
      for (let i = 0; i < n; i++) {
        const ai = Math.max(0, i - 1) * 3;
        const bi = Math.min(n - 1, i + 1) * 3;
        let dx = p[bi] - p[ai], dy = p[bi + 1] - p[ai + 1];
        const len = Math.hypot(dx, dy) || 1;
        dx /= len; dy /= len;
        const nx = -dy, ny = dx; // 法向 = 方向逆时针旋转 90°
        const ci = i * 3;
        const cx = p[ci], cy = p[ci + 1];
        const l: [number, number] = [cx + nx * halfW, cy + ny * halfW];
        const r: [number, number] = [cx - nx * halfW, cy - ny * halfW];
        if (prevL && prevR) {
          verts.push(prevL[0], prevL[1], h, prevR[0], prevR[1], h, l[0], l[1], h);
          verts.push(prevR[0], prevR[1], h, r[0], r[1], h, l[0], l[1], h);
        }
        prevL = l; prevR = r;
      }
    }
    if (verts.length === 0) return;
    const geom = this.track(new THREE.BufferGeometry());
    geom.setAttribute("position", new THREE.Float32BufferAttribute(verts, 3));
    const mat = this.track(
      new THREE.MeshBasicMaterial({
        color: style.color,
        transparent: false,
        depthWrite: false,
        side: THREE.DoubleSide,
      })
    );
    this.group.add(new THREE.Mesh(geom, mat));
  }

  // 箭头：由 position 定位、heading 绕 +z 旋转朝向、size 缩放。用 2D 平面箭头
  // 轮廓（杆 + 头，指向 +x），合批到单个 LineSegments 避免逐箭头 draw call。
  private buildArrows(layer: DecodedLayer, style: LayerStyle): void {
    // 单位箭头轮廓线段（长度 1，指向 +x），成对顶点表示线段。
    const shaft: number[][] = [
      [-0.5, 0], [0.3, 0],      // 杆
      [0.3, 0.18], [0.5, 0],    // 头右斜
      [0.3, -0.18], [0.5, 0],   // 头左斜
    ];
    const color = style.color;
    const verts: number[] = [];
    for (const item of layer.items) {
      if (!item.position) continue;
      const p = item.position;
      const yaw = item.heading;
      const cos = Math.cos(yaw), sin = Math.sin(yaw);
      // 缩放：优先用 size.x/size.y，缺省用默认长度 2m 兜底。
      const sx = item.size?.x || 2;
      const sy = item.size?.y || sx;
      for (const [lx, ly] of shaft) {
        const x = lx * sx, y = ly * sy;
        verts.push(
          p.x + x * cos - y * sin,
          p.y + x * sin + y * cos,
          p.z
        );
      }
    }
    if (verts.length === 0) return;
    const geom = this.track(new THREE.BufferGeometry());
    geom.setAttribute("position", new THREE.Float32BufferAttribute(verts, 3));
    const mat = this.track(new THREE.LineBasicMaterial({ color }));
    this.group.add(new THREE.LineSegments(geom, mat));
  }

  private buildPoints(layer: DecodedLayer, style: LayerStyle): void {
    // 点：用一个小 box 标记（自车框）。默认尺寸近似原样式 width/height。
    const size: Vec3 = { x: 4.5, y: 2.0, z: 1.6 };
    const edgeGeom = this.track(new THREE.EdgesGeometry(new THREE.BoxGeometry(1, 1, 1)));
    for (const item of layer.items) {
      if (!item.position) continue;
      // 颜色：与 box 一致——按类型上色（colorByType）时用 colorForType，否则用图层默认色。
      const color = style.colorByType ? colorForType(item.type) : style.color;
      const mat = this.track(new THREE.LineBasicMaterial({ color }));
      const mtx = boxMatrix(item.position, size, item.heading);
      const edges = new THREE.LineSegments(edgeGeom, mat);
      edges.applyMatrix4(mtx);
      this.group.add(edges);
      // 文字标签：优先用 item.text，否则回退 #id TYPE（与 perception box 一致）。
      const text = item.text && item.text.length > 0
        ? item.text
        : `#${item.id ?? "?"} ${perceptionTypeName(item.type)}`;
      const label = makeTextSprite(text, color);
      label.position.copy(v3(item.position, size.z + 0.6));
      const labelMat = label.material as THREE.SpriteMaterial;
      this.track(labelMat);
      // 注意:不 track labelMat.map —— 纹理由 textSpriteCache 全局复用,不能被 clear() 销毁。
this.group.add(label);
    }
  }

  private buildTexts(layer: DecodedLayer, style: LayerStyle): void {
    // 文本：用 canvas 纹理 Sprite 简化渲染（初始版本，清晰度有限）。
    for (const item of layer.items) {
      if (!item.position || !item.text) continue;
      const sprite = makeTextSprite(item.text, style.color);
      const p = v3(item.position, style.height + 0.4);
      sprite.position.copy(p);
      this.group.add(sprite);
    }
  }

  dispose(): void {
    this.clear();
  }
}

// 用 canvas 生成文字纹理 Sprite（billboard，始终朝向相机）。
// 文字纹理缓存：按 text|colorHex 复用 CanvasTexture。
// 播放中同一障碍物标签(#id TYPE)在帧间基本恒定,缓存后避免每帧重建 canvas
// (measureText/fillText 光栅化 + GPU 纹理上传)——这是标签数量较多时的主线程热点。
// 缓存的纹理为全局共享,故不进 LayerRenderer.disposables(否则 clear() 会误销毁复用中的纹理)。
interface CachedGlyph {
  tex: THREE.CanvasTexture;
  aspect: number; // w/h,用于 sprite 缩放
}
const textSpriteCache = new Map<string, CachedGlyph>();

function makeTextSprite(text: string, color: THREE.Color): THREE.Sprite {
  const key = text + "|" + color.getHexString();
  let glyph = textSpriteCache.get(key);
  if (!glyph) {
    const canvas = document.createElement("canvas");
    const ctx = canvas.getContext("2d")!;
    const font = "48px sans-serif";
    ctx.font = font;
    const w = Math.ceil(ctx.measureText(text).width) + 16;
    const h = 64;
    canvas.width = w;
    canvas.height = h;
    ctx.font = font;
    ctx.fillStyle = `rgb(${(color.r * 255) | 0},${(color.g * 255) | 0},${(color.b * 255) | 0})`;
    ctx.textBaseline = "middle";
    ctx.fillText(text, 8, h / 2);
    const tex = new THREE.CanvasTexture(canvas);
    tex.minFilter = THREE.LinearFilter;
    glyph = { tex, aspect: w / h };
    textSpriteCache.set(key, glyph);
  }
  // 每个 sprite 需独立 material(位置/scale 各异),但共享缓存纹理。
  const mat = new THREE.SpriteMaterial({ map: glyph.tex, transparent: true, depthWrite: false });
  const sprite = new THREE.Sprite(mat);
  // 世界尺度：字高约 0.7m，按宽高比缩放（原 1.5m 偏大，缩小便于观察场景）。
  sprite.scale.set(glyph.aspect * 0.7, 0.7, 1);
  return sprite;
}

// 依据 devicePixelRatio 同步 canvas 后备缓冲尺寸。
function syncCanvasSize(canvas: HTMLCanvasElement): { width: number; height: number } {
  const dpr = window.devicePixelRatio || 1;
  const width = Math.max(1, Math.round(canvas.clientWidth * dpr));
  const height = Math.max(1, Math.round(canvas.clientHeight * dpr));
  return { width, height };
}

// 场景渲染器：封装 Three.js 场景、相机、控制器与图层渲染。
class SceneRenderer {
  private renderer: THREE.WebGLRenderer;
  private scene = new THREE.Scene();
  private camera: THREE.PerspectiveCamera;
  private controls: OrbitControls;
  private world = new THREE.Group(); // 承载所有图层，做坐标系旋转
  private layers = new Map<string, LayerRenderer>();
  // 静态地图图层名集合（会话建立时经独立通道发一次、常驻渲染，不随逐帧刷新）。
  // 换文件时据此清理旧地图图层，避免与新文件地图叠加残留。
  private staticMapLayers = new Set<string>();
  // 点云渲染对象，以 point cloud id 为索引。放入 world（随 world 做 z↑→y↑ 坐标变换）。
  private pointClouds = new Map<string, THREE.Points>();
  // 显式被隐藏的图层集合。语义为「黑名单」：不在集合里的图层默认可见，
  // 避免后端产出的图层因未在 UI 白名单里而被误隐藏（图层名不匹配 bug）。
  private hidden = new Set<string>();
  // 后端下发的图层样式表，以 Frame 图层 key（LayerDef.source）为索引。
  // 未收到下发前为空，styleOf 回退内建默认。
  private layerStyles = new Map<string, LayerStyle>();
  // ego 在 three 世界坐标系下的最新位置（每帧数据到来时更新）。
  private egoTarget = new THREE.Vector3();
  private hasEgo = false;
  // 复用的临时向量：每帧 ego 与上一帧 ego 的位移差，避免频繁分配。
  private followDelta = new THREE.Vector3();
  // 相机模式：
  //   "follow" 跟随模式（默认）——相机锁定 ego，随主车平移，用户可绕 ego 旋转/缩放；
  //   "free"   自由浏览模式——相机不再跟随主车，切换为俯视视角，鼠标可自由拖拽平移浏览地图。
  private cameraMode: CameraMode = "follow";
  private raf = 0;
  // GPU 绘制耗时诊断：loop 里 renderer.render 的最近一次耗时（ms），供性能面板读取。
  private lastRenderMs = 0;
  private resizeObserver: ResizeObserver;

  getLastRenderMs(): number {
    return this.lastRenderMs;
  }
  constructor(canvas: HTMLCanvasElement) {
    this.renderer = new THREE.WebGLRenderer({ canvas, antialias: true });
    this.renderer.setClearColor(0x0a0e14, 1);
    const { width, height } = syncCanvasSize(canvas);
    this.renderer.setSize(width, height, false);

    this.camera = new THREE.PerspectiveCamera(55, width / height, 0.1, 2000);
    this.camera.position.set(-20, 20, 20);

    this.controls = new OrbitControls(this.camera, canvas);
    this.controls.enableDamping = true;
    this.controls.dampingFactor = 0.1;
    // 跟随模式下不允许屏幕空间平移（旋转中心锁 ego）；自由模式下开启平移以便拖拽浏览。
   // enablePan 会在 setCameraMode 里按模式切换，这里给出默认（跟随）。
    this.controls.enablePan = false;
    this.controls.screenSpacePanning = true;

    // world 旋转：proto z(上) → three y(上)。绕 X 轴 -90°。
    this.world.rotation.x = -Math.PI / 2;
    this.scene.add(this.world);

    // 光照 + 地面网格。
    this.scene.add(new THREE.AmbientLight(0xffffff, 0.8));
    const dir = new THREE.DirectionalLight(0xffffff, 0.6);
    dir.position.set(50, 100, 50);
    this.scene.add(dir);
    const grid = new THREE.GridHelper(400, 80, 0x2a3240, 0x1a2029);
    this.scene.add(grid);

    this.resizeObserver = new ResizeObserver(() => this.resize());
    this.resizeObserver.observe(canvas);

    const loop = () => {
      this.raf = requestAnimationFrame(loop);
      this.controls.update();
      const t0 = performance.now();
      this.renderer.render(this.scene, this.camera);
      // EWMA 平滑 GPU 绘制耗时（render 是同步调用，可直接测；不含 GPU 异步完成）。
      this.lastRenderMs = this.lastRenderMs * 0.9 + (performance.now() - t0) * 0.1;
    };
    this.raf = requestAnimationFrame(loop);
  }

  private resize(): void {
    const canvas = this.renderer.domElement;
    const { width, height } = syncCanvasSize(canvas);
    this.renderer.setSize(width, height, false);
    this.camera.aspect = width / height;
    this.camera.updateProjectionMatrix();
  }

  // 应用后端下发的图层样式定义（LAYER_DEFS）。以 def.source 为渲染索引键。
  setLayerStyles(defs: LayerDef[]): void {
    this.layerStyles.clear();
    for (const def of defs) {
      this.layerStyles.set(def.source, styleFromDef(def));
    }
  }

  // 查图层样式：优先后端下发，其次内建默认，最后灰色兜底。
  private styleOf(layerId: string): LayerStyle {
    return (
      this.layerStyles.get(layerId) ??
      DEFAULT_LAYER_STYLES[layerId] ??
      { color: new THREE.Color(0.7, 0.7, 0.7), colorByType: false, height: 0 }
    );
  }

  setLayerVisible(layerId: string, v: boolean): void {
    if (v) this.hidden.delete(layerId);
    else this.hidden.add(layerId);
    const lr = this.layers.get(layerId);
    if (lr) lr.group.visible = v;
    const pts = this.pointClouds.get(layerId);
    if (pts) pts.visible = v;
  }

  getCameraMode(): CameraMode {
    return this.cameraMode;
  }

  // 切换相机模式：
  //   -> "free"：进入自由浏览。相机切到 ego 正上方的俯视视角（无旋转跟随），
  //      开启 OrbitControls 平移（enablePan），用户可用鼠标拖拽平移浏览整张地图与渲染对象。
  //   -> "follow"：回到跟随。以当前 ego 位置为旋转中心，把相机放回后上方观察位并锁定平移，
  //      后续帧随 ego 位移同步平移。
  setCameraMode(mode: CameraMode): void {
    if (mode === this.cameraMode) return;
    this.cameraMode = mode;
    if (mode === "free") {
      // 自由浏览：俯视 + 允许平移。以当前 ego（若有）为俯视中心，否则用当前 target。
      const center = this.hasEgo ? this.egoTarget.clone() : this.controls.target.clone();
      this.controls.target.copy(center);
      // 相机置于中心正上方，形成俯视图（略带偏移避免万向锁）。
      this.camera.position.set(center.x, center.y + 120, center.z + 0.001);
      this.controls.enablePan = true;
      this.controls.screenSpacePanning = true;
    } else {
      // 跟随模式：关闭平移，旋转中心锁 ego，相机回到后上方观察位。
      this.controls.enablePan = false;
      const center = this.hasEgo ? this.egoTarget.clone() : this.controls.target.clone();
      this.controls.target.copy(center);
      this.camera.position.copy(center).add(new THREE.Vector3(-20, 20, 20));
    }
    this.controls.update();
  }
  // 渲染一帧：按图层名重建几何，更新相机跟随目标。
  renderFrame(frame: DecodedFrame): void {
    for (const [name, layer] of Object.entries(frame.layers)) {
      let lr = this.layers.get(name);
      if (!lr) {
        lr = new LayerRenderer(name);
        // 默认可见；仅当被显式隐藏时才关闭。
        lr.group.visible = !this.hidden.has(name);
        this.layers.set(name, lr);
        this.world.add(lr.group);
      }
      lr.build(layer, this.styleOf(name));
    }
    // 点云图层：按 id 建/更新 THREE.Points（BufferGeometry position 属性）。
    this.renderPointClouds(frame.pointClouds);
    // 相机跟随 ego：把 proto 锚点转到 three 世界坐标（应用 world 旋转）。
    // 首帧把相机放到 ego 后上方并将 target 对准 ego；后续帧按 ego 位移量 delta
    // 同步平移 target 与 camera，保持用户当前观察角度/距离，且旋转中心始终锁在 ego 上。
    if (frame.egoValid) {
      this.world.updateMatrixWorld();
      const local = new THREE.Vector3(frame.egoAnchor.x, frame.egoAnchor.y, frame.egoAnchor.z);
      const worldPos = local.applyMatrix4(this.world.matrixWorld);
      if (this.cameraMode === "follow") {
        if (!this.hasEgo) {
          // 首帧：把相机放到 ego 后上方的默认俯视位，并将 target 对准 ego。
          this.controls.target.copy(worldPos);
          this.camera.position.copy(worldPos).add(new THREE.Vector3(-20, 20, 20));
        } else {
          // 后续帧：ego 移动了 delta，则把 target 与 camera 同时平移 delta，
          // 保持用户当前的观察角度/距离不变，同时旋转中心始终锁在 ego 上。
          this.followDelta.copy(worldPos).sub(this.egoTarget);
          if (this.followDelta.lengthSq() > 1e-12) {
            this.controls.target.add(this.followDelta);
            this.camera.position.add(this.followDelta);
          }
        }
        this.controls.update();
      }
      // 自由模式不移动相机，但仍记录 ego 位置：一是首帧标记，二是切回跟随时以此为基准。
      this.hasEgo = true;
      this.egoTarget.copy(worldPos);
    }
  }

  // 渲染「静态地图」图层（会话建立时经独立通道发一次）。与 renderFrame 同样按图层名
  // 建 LayerRenderer 常驻，但这些图层不随逐帧刷新、也不进逐帧缓存，避免地图逐帧冗余
  // 导致的全量缓存膨胀（实测 750 帧 ~1GB）。换文件时经 clearStaticMap 清理。
  renderStaticMap(layers: DecodedFrame["layers"]): void {
    for (const [name, layer] of Object.entries(layers)) {
      let lr = this.layers.get(name);
      if (!lr) {
        lr = new LayerRenderer(name);
        lr.group.visible = !this.hidden.has(name);
        this.layers.set(name, lr);
        this.world.add(lr.group);
      }
      lr.build(layer, this.styleOf(name));
      this.staticMapLayers.add(name);
    }
  }

  // 清理所有静态地图图层（换文件时调用）：从场景移除并释放几何，避免新旧地图叠加残留。
  clearStaticMap(): void {
    for (const name of this.staticMapLayers) {
      const lr = this.layers.get(name);
      if (lr) {
        this.world.remove(lr.group);
        lr.dispose();
        this.layers.delete(name);
      }
    }
    this.staticMapLayers.clear();
  }

  // 按点云 id 建/更新 THREE.Points。有 rgb 用顶点色，否则固定色（后续可按 intensity/height 扩展）。
  private renderPointClouds(clouds: Record<string, DecodedPointCloud>): void {
    const seen = new Set<string>();
    for (const [id, pc] of Object.entries(clouds)) {
      seen.add(id);
      const count = pc.xyz.length / 3;
      let pts = this.pointClouds.get(id);
      if (!pts) {
        const geom = new THREE.BufferGeometry();
        const mat = new THREE.PointsMaterial({ size: 0.06, sizeAttenuation: true });
        pts = new THREE.Points(geom, mat);
        pts.frustumCulled = false;
        pts.visible = !this.hidden.has(id);
        this.pointClouds.set(id, pts);
        this.world.add(pts);
      }
      const geom = pts.geometry as THREE.BufferGeometry;
      // position：复用解码得到的 packed float（每 3 个一点，坐标系由 world 旋转统一处理）。
      const posAttr = geom.getAttribute("position") as THREE.BufferAttribute | undefined;
      if (!posAttr || posAttr.count !== count) {
        geom.setAttribute("position", new THREE.BufferAttribute(pc.xyz, 3));
      } else {
        (posAttr.array as Float32Array).set(pc.xyz);
        posAttr.needsUpdate = true;
      }
      const mat = pts.material as THREE.PointsMaterial;
      if (pc.rgb && pc.rgb.length === count * 3) {
        geom.setAttribute("color", new THREE.BufferAttribute(pc.rgb, 3));
        mat.vertexColors = true;
      } else {
        if (geom.getAttribute("color")) geom.deleteAttribute("color");
        mat.vertexColors = false;
      }
      mat.needsUpdate = true;
      geom.setDrawRange(0, count);
      geom.computeBoundingSphere();
    }
    // 本帧未出现的点云：清空绘制（保留对象，避免频繁重建）。
    for (const [id, pts] of this.pointClouds) {
      if (!seen.has(id)) (pts.geometry as THREE.BufferGeometry).setDrawRange(0, 0);
    }
  }

  dispose(): void {
    cancelAnimationFrame(this.raf);
    this.resizeObserver.disconnect();
    this.controls.dispose();
    for (const lr of this.layers.values()) lr.dispose();
    for (const pts of this.pointClouds.values()) {
      pts.geometry.dispose();
      (pts.material as THREE.Material).dispose();
    }
    this.renderer.dispose();
  }
}

export { SceneRenderer };

// -----------------------------------------------------------------------------
// Three.js 引擎：装配 SceneRenderer + FrameStream 播放控制。
// 播放控制逻辑对齐原 createStreamEngine：onInfo 自动播放、rAF 推进时间、
// setPaused 末尾回绕修复、open/openByName/openRecord/upload。
// -----------------------------------------------------------------------------
// 【方案A·纯内存缓存】换新文件(open/openByName/openRecord/uploadFile)或换包时经 clearCache()
// 清空内存缓存(cache.length = 0)并自增 cacheGeneration 作废在途解码，确保「换包全清」不串数据。
// 不再有任何 IndexedDB 落盘/删库动作——全部帧仅驻留内存。

// 在未缓存 seek 的目标帧回填完成前，playhead 必须停留在目标位置，确保高清流收到
// 正确的目标时间；普通播放才允许按当前缓存末尾封顶。
export function resolvePlaybackTime(
  projectedTime: number,
  loadedMaxTime: number,
  pendingSeekTarget: number | null
): number {
  if (pendingSeekTarget !== null) return pendingSeekTarget;
  return Math.min(projectedTime, loadedMaxTime);
}

export function shouldReportPlayhead(
  now: number,
  lastAt: number,
  time: number,
  lastTime: number,
  scrubbing: boolean,
  force = false
): boolean {
  if (force) return true;
  // 拖动只按时间节流，不能因位置每次跳变而退化成 60Hz 上报。
  if (scrubbing) return now - lastAt >= 120;
  return now - lastAt > 80 || Math.abs(time - lastTime) > 0.05;
}

export function createExclusiveRawDataSubscription(
  send: (channel: string, enabled: boolean) => void
) {
  let currentChannel: string | null = null;
  return {
    update(channel: string, enabled: boolean): void {
      if (!enabled) {
        if (channel !== currentChannel) return;
        send(channel, false);
        currentChannel = null;
        return;
      }
      if (channel === currentChannel) return;
      if (currentChannel !== null) send(currentChannel, false);
      send(channel, true);
      currentChannel = channel;
    },
  };
}

export async function createThreeEngine(opts: ThreeEngineOptions): Promise<EngineApi> {
  // 先加载 proto codec（fetch /frame.proto），再建场景。
  await loadFrameCodec();
  const scene = new SceneRenderer(opts.canvas);
  let duration = 0;
  let frameCount = 0;
  let time = 0;
  // 上一次已处理的 STREAM_INFO 代次。后端每次 Open 才自增 generation 并下发 STREAM_INFO；
  // 仅当代次变化(真正的新流/重开)才复位播放位置与清缓存。
  let lastInfoGeneration = -1;
  let paused = true;
  let speed = 1;
  let disposed = false;
  let advanceRaf = 0;
  let lastTick = performance.now();
  let lastCharts: ChartData[] = [];
  let dataMode: DataMode = null;

  // 解码 Worker 池：把大帧 protobuf 解码移出主线程，避免阻塞 rAF 渲染（此前 ~23Hz 瓶颈）。
  const decoderPool = new FrameDecoderPool();
  // 【方案A·纯内存缓存】已彻底移除 IndexedDB 落盘链路：全部帧只缓存在内存，
  // seek 只消费内存缓存，未命中则向后端请求从目标位置续供帧。换包由 clearCache()+
  // cacheGeneration 自增负责作废旧数据，无需再维护数据源命名空间标识。

  // --- 前端帧缓存（浏览器端缓存已收到的帧，播放/暂停/进度全由前端本地时钟主导）---
  // 参考 webmonitor：后端仅按序供数填充缓存，前端 rAF 时钟推进 time 后从缓存取帧渲染。
  // 这样暂停即冻结本地时钟（画面立即停、进度条停、速率照常显示），不再依赖后端推帧节奏。
  //
  // cache：按时间戳升序排列的已解码帧；全部帧常驻内存，frameBytes/cacheBytes 仅用于
  // 观测实际占用。不能按帧数或字节静默裁头，否则会破坏任意已缓存位置可 seek 的契约。
  const cache: DecodedFrame[] = [];
  const frameBytes: number[] = [];        // 与 cache 一一对应的单帧估算字节，裁剪时同步维护
  let cacheBytes = 0;                      // 当前缓存总字节（frameBytes 之和）
  // 全量常驻模式：不设帧数或字节裁剪阈值，否则缓存只剩尾部而 loadedMaxTime 仍指向
  // 整段末端，进度条前段看似可 seek、实际 frameAt 无帧。cacheBytes 仅用于状态观测。
  let loadedMaxTime = 0; // 缓存中最大帧时间戳（后端已推进度），仅供进度条展示，绝不封顶 playhead
  let renderedTime = -1; // 上一次实际渲染的帧时间戳，避免重复渲染同一帧
  // 【xplayer index 寻址模型移植】playhead 是唯一权威播放位置（秒），对外仍以 time 别名暴露。
  // 参考 xplayer-ts：allInformation[index] 数组下标即位置，JumpIndex(index) 直接按索引渲染，
  // 位置只被"播放时钟推进 / seek / 拖动"单向决定，渲染只读不写、后端回填从不改位置。
  // 由此天然根治两 bug：
  //   1) 长距离拖动无响应 —— seek/拖动直接把 playhead 设到目标，未缓存则节流回填，
  //      命中即渲染，位置不再被 loadedMaxTime 封顶拉回。
  //   2) 暂停恢复不从暂停位续播 —— 暂停只停时钟，playhead 冻结不动，后端回填/裁剪都
  //      不碰 playhead，恢复播放必从暂停处续播。
  // 拖动期间只保存最新目标；advance 每轮至多消费一个目标，过期目标不排队。
  let scrubbing = false;
  let pendingPreviewTime: number | null = null;
  let backfillAt = 0;      // 上次未命中触发后端回填的时刻（节流用，~120ms/次）
  let backfillTarget = -1; // 上次回填请求的目标位置（避免同位置重复请求）
  // 未缓存 seek的目标。目标附近普通帧到达前固定 playhead，避免被旧 loadedMaxTime 拉回，
  // 从而让 playhead 上报持续驱动高清线程解码正确位置。
  let pendingSeekTarget: number | null = null;
  // 上次向后端上报 playhead 的时刻与位置（节流用，~200ms/次），驱动大数据预解码窗口跟随。
  let playheadReportAt = 0;
  let playheadReportTime = -1;
  // 缓存代次：换包/清缓存时自增；异步解码回来的旧代次帧直接丢弃，避免污染新数据源缓存。
  let cacheGeneration = 0;



  // --- 性能指标采集（供帧率组件分析播放卡顿）---
  // rAF 帧率：统计窗口内 advance 调用次数 / 耗时。渲染帧率：窗口内真正 renderFrame 次数。
  let statFps = 0;         // 最近窗口 rAF 帧率（Hz）
  let statRenderFps = 0;   // 最近窗口实际渲染帧率（Hz，去重后≈数据帧率）
  let statWindowStart = performance.now();
  let statRafCount = 0;    // 窗口内 advance 次数
  let statRenderCount = 0; // 窗口内实际 renderFrame 次数
  let starving = false;    // 播放中 time 被 loadedMaxTime 封顶（后端供数跟不上）
  let statBuildMs = 0;     // scene.renderFrame（几何 build）耗时 EWMA（ms）
  let diagPlayCount = 0;     // [DIAG] type1 播放帧到达计数
  let diagPrefetchCount = 0; // [DIAG] type9 预取帧到达计数
  // [DIAG-OOM] cache 增长诊断:验证 OOM 是否源于全量常驻(cache 无上限)及单帧字节量级。
  let diagInsertCount = 0;    // insertFrame 累计调用次数
  let diagFrameBytesMin = Number.POSITIVE_INFINITY;
  let diagFrameBytesMax = 0;
  let diagFrameBytesSum = 0;  // 用于算平均

  // 按时间戳有序插入（去重）。后端一般顺序推帧，末尾快速路径覆盖多数情况。
  // frameBytes 与 cache 同步维护（同增同删同覆盖），cacheBytes 为其累加和，供字节数背压。
  const insertFrame = (frame: DecodedFrame) => {
    const t = frame.t;
    const bytes = estimateFrameBytes(frame);
    if (cache.length === 0 || t > cache[cache.length - 1].t) {
      cache.push(frame);
      frameBytes.push(bytes);
      cacheBytes += bytes;
    } else {
      let lo = 0;
      let hi = cache.length;
      while (lo < hi) {
        const mid = (lo + hi) >> 1;
        if (cache[mid].t < t) lo = mid + 1;
        else hi = mid;
      }
      if (cache[lo] && cache[lo].t === t) {
        // 【缓存帧合并语义】同一时间戳的帧已在缓存中：这几乎只发生在“补发/重发”场景。
        // 大数据（images/rawData）已迁出全量缓存、改走 BigData 独立流按 playhead 实时持有，
        // 故此处不再合并 images/rawData（避免大数据被复制进常驻缓存造成 N 倍冗余）。
        // 仅保留 pointClouds 合并——点云仍在常规缓存内，补发时需并入旧帧几何。
        const old = cache[lo];
        const merged: DecodedFrame = {
          ...old,
          pointClouds: { ...old.pointClouds, ...frame.pointClouds },
        };
        const mergedBytes = estimateFrameBytes(merged);
        // 先扣旧帧字节,再计入合并后帧字节。
        cacheBytes += mergedBytes - frameBytes[lo];
        cache[lo] = merged;
        frameBytes[lo] = mergedBytes;
      } else {
        cache.splice(lo, 0, frame);
        frameBytes.splice(lo, 0, bytes);
        cacheBytes += bytes;
      }
    }
    if (t > loadedMaxTime) loadedMaxTime = t;
    // 全量内存模式禁止裁掉历史帧。loadedMaxTime 只表示最新帧，若裁头后仍以它作为
    // 可拖动上界，会造成滑块可进入已不存在的区间，最终只剩缓存尾部能预览。
    // 内存风险由 UI 指标 cacheBytesMB 明示，不再用静默破坏 seek 语义的方式兜底。

    // [DIAG-OOM] 纯观测:不改缓存行为,仅暴露 cache 增长曲线与单帧字节量级,
    // 供实测确认 16GB 是否源于「全量常驻 × 单帧字节」。每 200 帧汇总一次。
    diagInsertCount++;
    diagFrameBytesSum += bytes;
    if (bytes < diagFrameBytesMin) diagFrameBytesMin = bytes;
    if (bytes > diagFrameBytesMax) diagFrameBytesMax = bytes;
    if (diagInsertCount % 200 === 0) {
      const avg = diagFrameBytesSum / diagInsertCount;
      const layerItems = Object.values(frame.layers).reduce((n, l) => n + l.items.length, 0);
     // 按图层名拆分单帧字节，直接定位是哪个图层(planning/perception/prediction/...)吃内存。
      // 每图层字节 = items 数 * (96 基础 + position 64 + size 64) + 所有 item.points.byteLength 之和。
      const perLayer = Object.entries(frame.layers)
        .map(([name, l]) => {
          let b = 0;
          let pts = 0;
          for (const it of l.items) {
            b += 96;
            if (it.position) b += 64;
            if (it.size) b += 64;
            b += it.points.byteLength;
            pts += it.points.byteLength / 12; // packed Float32Array，每点 12B
          }
          return `${name}(items=${l.items.length},pts=${Math.round(pts)},${(b / 1024).toFixed(1)}KB)`;
        })
        .join(" ");
      opts.onStatus?.(
        `[diag-oom] cache.len=${cache.length} cacheBytes=${(cacheBytes / 1_000_000).toFixed(1)}MB ` +
          `estFrame(min/avg/max)=${(diagFrameBytesMin / 1024).toFixed(0)}/${(avg / 1024).toFixed(0)}/${(diagFrameBytesMax / 1024).toFixed(0)}KB ` +
          `lastFrame layers=${Object.keys(frame.layers).length} items=${layerItems} ` +
          `pc=${Object.keys(frame.pointClouds).length} img=${Object.keys(frame.images).length} | ${perLayer}`,
      );
    }
  };

  // 取时间戳 <= t 的最近一帧（缓存有序，二分）。用于按本地时钟渲染当前应显示帧。
  // 【修复 seek 无效】t 早于缓存最旧帧（seek 后回填等待期/头部被内存背压裁掉）时
  // 返回 null 而非 cache[0]：旧实现会把远处的尾部帧渲染出去，renderedTime 被顶回
  // seek 前位置（getTime/画面均"弹回去"，表现为 seek 无效）。
  const frameAt = (t: number): DecodedFrame | null => {
    if (cache.length === 0) return null;
    let lo = 0;
    let hi = cache.length - 1;
    let ans = -1;
    while (lo <= hi) {
      const mid = (lo + hi) >> 1;
      if (cache[mid].t <= t) {
        ans = mid;
        lo = mid + 1;
      } else {
        hi = mid - 1;
      }
    }
    return ans >= 0 ? cache[ans] : null;
  };

  // 大数据（图像/RawData）当前帧持有者：getGen 复用传输层代次（stream.currentGeneration()，
  // seek 时自增，与后端大数据帧头 gen 同源）。落后代次的在途大数据帧会被 store.handle 丢弃。
  // getGen 为惰性闭包：仅在收到大数据帧的异步回调中调用，此时 stream 已完成初始化。
  const bigDataStore = createBigDataStore(() => stream.currentGeneration());

  // 【缩略图缓存】进度条拖动预览用：全量缓存所有已生成的低清缩略图（300×180 JPEG），
  // 按 channel 分组、按 tSec 升序，findNearest 二分查最近一张。代次同源 bigDataStore。
  const thumbnailStore = createThumbnailStore(() => stream.currentGeneration());

  // 【RawData 解码链路】type-12 defs → decoder.setDefinitions；被 bigDataStore 接受的 type-11
  // raw 帧 → decoder.decode → panelReducer → onRawDataPanelUpdate。wiring 抽出为纯逻辑单元
  // （见 rawDataWiring.ts），依赖注入 decoder / bigDataStore / 代次 / 面板回调。
  const rawDataDecoder = new RawDataDecoder();
  const rawDataWiring = createRawDataWiring({
    decoder: rawDataDecoder,
    store: bigDataStore,
    currentGeneration: () => stream.currentGeneration(),
    isPaused: () => paused,
    onPanelUpdate: (s) => opts.onRawDataPanelUpdate?.(s),
  });

  const clearCache = () => {
    cache.length = 0;
    frameBytes.length = 0;
    cacheBytes = 0;
    loadedMaxTime = 0;
    renderedTime = -1;
    pendingPreviewTime = null;
    // 自增代次：此刻起在途的旧代次异步解码结果作废，回来后按代次判定丢弃。
    cacheGeneration++;
    // 【方案C·静态地图】换文件/重开流会清缓存,此时也清掉常驻静态地图,
    // 避免旧地图与新流地图叠加。新流 EmitStreamInfoLocked 会重发 type 10 地图。
    scene.clearStaticMap();
    // 【大数据流】换文件/重开流清缓存时同步清空大数据当前帧并 revoke blobUrl，
    // 避免旧流图像残留到新流。代次自增亦会丢弃在途旧代次大数据帧。
    bigDataStore.clear();
    // 【缩略图缓存】换流时同步清空并 revoke 全部缩略图 blobUrl，避免旧流残留。
    thumbnailStore.clear();
    // 【RawData】换流/seek 清缓存时同步 decoder.setGeneration + 清面板内容，
    // 使在途旧代次 raw 解码结果作废、面板不残留旧流内容。
    rawDataWiring.onClearCache();
    // 【方案A·纯内存】仅清空内存热缓存,无 IndexedDB 落盘可清。
  };

  // 【方案A·全量常驻】不再有"seek 后裁掉过期帧"的 trimCacheBefore：整包帧常驻内存,
  // 任意已缓存位置都能立即命中渲染(拖动实时预览)。裁剪仅由 insertFrame 尾部 OOM 硬安全阀兜底。

  const handlers: TransportHandlers = {
    onOpen: () => {
      opts.onStatus?.("已连接，正在读取文件列表");
      stream.listFiles();
      if (opts.sourceFile) stream.open(opts.sourceFile);
    },
    onInfo: (info) => {
      // 同一流的重复/迟到 STREAM_INFO(代次未变)：仅刷新时长/帧数等元信息，
      // 绝不复位 time / 清缓存 / 强制起播，否则分段预取期间画面会被打回零。
      const sameStream = info.generation === lastInfoGeneration;
      duration = info.durationSec;
      frameCount = info.frameCount;
      dataMode = info.dataMode ?? null;
      if (sameStream) {
        opts.onStatus?.(`已加载 ${info.frameCount} 帧`);
        return;
      }
      // 新流/重开(代次变化)：复位播放位置并清缓存，从头开始播放。
      lastInfoGeneration = info.generation;
      time = 0;
      clearCache();
      paused = false;
      lastTick = performance.now();
      stream.send({ type: "setPaused", paused: false });
      // 【方案A·全量常驻】新流建立后立即启用后端全速预取(type 9)：起独立线程从帧0
      // 全速下发,与播放帧并行把整包帧灌入内存滑窗。不发则全速预取通道从不启动,
      // 缓存只能跟随播放进度慢速累积,与"全量落盘常驻内存"设计意图不符。指令幂等。
      stream.send({ type: "startPrefetch" });
      opts.onStatus?.(`已加载 ${info.frameCount} 帧开始播放`);
    },
    onFrame: (_seq, bytes) => {
      // 【方案A·纯内存】收到播放帧后直接送 Worker 池解码 → insertFrame 进内存滑窗，
      // 不再落盘。缓存进度完全由 loadedMaxTime（内存最新帧时刻）表征。
      const gen = cacheGeneration;
      const seq = Number(_seq);
      decoderPool
        .decode(seq, bytes)
        .then((frame) => {
          if (disposed || gen !== cacheGeneration) return;
          if (!Number.isFinite(frame.t)) return;
          // [DIAG-IMG] 补发帧诊断：任何带 image 的帧到达都打印，暴露补发是否到达、
          // 其 t 与当前 renderedTime 是否一致、images 携带哪些通道。补发帧稀少故不采样。
          const imgKeys = Object.keys(frame.images);
          if (imgKeys.length > 0) {
            opts.onStatus?.(`[diag-img][onFrame] seq=${seq} t=${frame.t.toFixed(3)} rendered=${renderedTime.toFixed(3)} imgCh=[${imgKeys.join(",")}] eq=${frame.t === renderedTime}`);
          }
          // [DIAG] 播放帧(type1)到达实证：确认前端是否真收到 play 帧及其 t 值。
          if ((diagPlayCount++ % 50) === 0)
            opts.onStatus?.(`[diag][onFrame type1] n=${diagPlayCount} seq=${seq} t=${frame.t.toFixed(3)} loadedMax=${loadedMaxTime.toFixed(3)}`);
          insertFrame(frame);
          // 【面板不显示 bug 修复·缓存帧补图】勾选图像通道后，后端无条件补发“当前显示帧”
          // (带 image)。该帧 insertFrame 覆盖缓存中同 t 的“无 image”旧帧后，若它正是当前
          // 已渲染帧(t===renderedTime)，渲染 tick 因几何去重不重绘、renderFrameNow 又早退，
          // onImage 本会漏触发。故这里对“当前显示帧的补发”直接提取 image 即时上屏(带去重，
          // 避免同 t 帧重复重建 Blob)。非当前帧的补发由后续渲染 tick 命中时经 renderFrameNow 提取。
          if (frame.t === renderedTime && frame.t !== lastImageTime &&
              opts.onImage && Object.keys(frame.images).length > 0) {
            lastImageTime = frame.t;
            emitImages(frame);
          }
        })
        .catch((error) => {
          if (disposed || gen !== cacheGeneration) return;
          opts.onStatus?.(error instanceof Error ? error.message : "解码帧失败", true);
        });
    },
    onPrefetchFrame: (_seq, bytes) => {
      // 全速预取帧(type 9)：封包同普通帧，消费路径相同——解码后注入内存滑窗，
      // 由 loadedMaxTime 表征缓存进度。语义上是后台全量落盘的非实时帧。
      const gen = cacheGeneration;
      const seq = Number(_seq);
      decoderPool
        .decode(seq, bytes)
        .then((frame) => {
          if (disposed || gen !== cacheGeneration) return;
          if (!Number.isFinite(frame.t)) return;
          // [DIAG] 预取帧(type9)到达实证。
          if ((diagPrefetchCount++ % 100) === 0)
            opts.onStatus?.(`[diag][onPrefetch type9] n=${diagPrefetchCount} seq=${seq} t=${frame.t.toFixed(3)} loadedMax=${loadedMaxTime.toFixed(3)}`);
          insertFrame(frame);
        })
        .catch((error) => {
          if (disposed || gen !== cacheGeneration) return;
          opts.onStatus?.(error instanceof Error ? error.message : "解码预取帧失败", true);
        });
    },
    // 【方案C·静态地图独立通道】会话建立时后端单发一次地图(type 10)。
    // 解码后直接常驻渲染,不进逐帧 cache(不 insertFrame、不判 frame.t),
    // 从根上避免轻图静态地图被逐帧复制 750 次撑爆 buffer(原 1054MB 根因)。
    onStaticMap: (_seq, bytes) => {
      const gen = cacheGeneration;
      decoderPool
        .decode(Number(_seq), bytes)
        .then((frame) => {
          if (disposed || gen !== cacheGeneration) return;
          scene.renderStaticMap(frame.layers);
        })
        .catch((error) => {
          opts.onStatus?.(error instanceof Error ? error.message : "解码静态地图失败", true);
        });
    },
    onBigData: (frame) => {
      // 大数据独立流：交给持有者做代次校验 + 当前帧替换（不进常规缓存），
      // 再通知上层某 channel 有新数据可拉取。
      if (frame.kind === "thumbnail") {
        // 缩略图走全量缓存（拖动预览专用），不覆盖当前高清帧。
        thumbnailStore.handle(frame);
      } else {
        bigDataStore.handle(frame);
        // RawData：帧已被 bigDataStore 做代次校验后接受，交给 wiring 触发解码（仅当
        // 该通道被选中且订阅开启时才实际 decode；image 分支不受影响）。
        if (frame.kind === "raw") {
          rawDataWiring.onRawFrame(frame.channel);
        }
      }
      opts.onBigDataUpdate?.(frame.channel, frame.kind);
    },
    onFileList: (files) => opts.onFileList?.(files),
    onChartDefs: (defs) => opts.onChartDefs?.(defs),
    onLayerDefs: (defs) => {
      scene.setLayerStyles(defs);
      opts.onLayerDefs?.(defs);
    },
    onImageDefs: (defs) => opts.onImageDefs?.(defs),
    onRawDataDefs: (defs) => {
      // RawData 通道定义：喂给 decoder 建 registry + 面板初始化可选通道，再透传上层建复选框。
      rawDataWiring.handleDefs(defs);
      opts.onRawDataDefs?.(defs);
    },
    onUploadStatus: (state) => {
      const text =
        state === "ready"
          ? "上传通道就绪，正在传输"
          : state === "processing"
            ? "上传完成，正在解码"
            : "上传解码完成";
      opts.onStatus?.(text);
    },
    onClose: () => opts.onStatus?.("数据服务连接已关闭", true),
    onError: (error) =>
      opts.onStatus?.(error instanceof Error ? error.message : "数据服务连接错误", true),
  };
  const stream: Transport =
    opts.transportMode === "ffi"
      ? new FfiTransport(handlers)
      : new FrameStream(opts.wsUrl ?? "/ws", handlers);
  const rawDataSubscription = createExclusiveRawDataSubscription(
    (channel, enabled) => stream.subscribeRawData(channel, enabled)
  );
  stream.connect();

  // 已回调过 image 的帧 t，避免同一带 image 帧重复触发 onImage（Blob 反复重建）。
  let lastImageTime = Number.NaN;
  const emitImages = (frame: DecodedFrame) => {
    if (!opts.onImage) return;
    for (const [channel, img] of Object.entries(frame.images)) {
      // [DIAG-IMG] 确认 onImage 是否真被调用及 channel/尺寸/字节。
      opts.onStatus?.(`[diag-img][emit] ch=${channel} bytes=${img.data?.length ?? 0} ${img.width}x${img.height}`);
      if (img.data && img.data.length > 0) {
        opts.onImage(channel, img.data, img.width, img.height);
      }
    }
  };
  const renderFrameNow = (frame: DecodedFrame) => {
    // range input 可能比数据帧频率高；同一帧几何无需重复重建场景。
    // 【缓存帧补图 bug 修复】但 image 不能随几何去重一起早退：勾选相机后 frameAt(time)
    // 命中的可能是“已渲染过几何、但此刻才被后端补发带 image 覆盖”的当前帧(t===renderedTime)。
    // 若在此 return，image 永不上屏、相机面板不显示。故几何去重时仍尝试提取该帧 image。
    if (frame.t === renderedTime) {
      if (frame.t !== lastImageTime && Object.keys(frame.images).length > 0) {
        lastImageTime = frame.t;
        emitImages(frame);
      }
      return;
    }
    scene.renderFrame(frame);
    lastCharts = frame.charts;
    renderedTime = frame.t;
    if (Object.keys(frame.images).length > 0) {
      lastImageTime = frame.t;
      emitImages(frame);
    }
  };


  // 纯推算：给定当前真实时刻 now，算出播放应处的 playhead（不写任何状态）。
  // 【xplayer 模型】位置只被本地时钟单向推进，封顶 duration 即可 —— 绝不再封顶
  // loadedMaxTime（那是"seek 无效/画面弹回"的根源）。未缓冲区间由 advance 负责
  // "保持上一帧 + 节流回填"，位置照常前进，不被回填进度拖回。
  const projectTime = (now: number) => {
    if (paused) return time;
    // 拖动/正式 seek 已把 time 直接置到目标（xplayer JumpIndex 语义），
    // 拖动期间冻结时钟不推进（避免拖动位置被时钟带偏）。
    if (scrubbing) return time;
    const next = time + ((now - lastTick) / 1000) * speed;
    return Math.min(duration, next);
  };

  // 【xplayer 移植·统一回填】playhead 落在未缓存区间时，节流请求后端从该位置回填。
  // 不阻塞、不改 playhead —— 后端供帧经 Worker 解码 insertFrame 后，下一帧 advance
  // 的 frameAt(time) 自然命中并渲染，画面追上位置。节流(~120ms + 位置去重)防请求风暴。
  const backfillIfMissing = (target: number) => {
    const now = performance.now();
    if (now - backfillAt > 120 && Math.abs(target - backfillTarget) > 0.3) {
      backfillAt = now;
      backfillTarget = target;
      // 【方案A·全量常驻】此处不再 trimCacheBefore(target-2)：整包帧常驻内存,
      // 拖动/seek 时任意已缓存位置都要能立即命中渲染实现实时预览。旧实现每次回填
      // 都裁掉 target-2 之前的帧,导致拖动途中缓存被反复裁空,只有拖到刚回填的
      // 1~2s 窗口才命中 —— 正是"拖动只在最后1秒才渲染画面"的根因。裁剪只由
      // insertFrame 尾部的 OOM 硬安全阀在极端内存超标时兜底。
      stream.seek(target);
      // 后端可能处于自动暂停(上一轮播到末尾)；显式恢复供帧。前端 paused 仅控本地时钟，
      // 因此暂停画面也能继续回填缓存，不影响 playhead 冻结。
      stream.send({ type: "setPaused", paused: false });
    }
  };

  const advance = () => {
    if (disposed) return;
    const now = performance.now();
    // 【xplayer 单一真相模型】advance 只做两件事：推进 playhead(仅播放态) + 按 playhead
    // 取帧渲染(只读不写)。渲染取到的帧绝不回改 playhead —— 位置只被时钟/seek/拖动决定。
    // 由此：暂停冻结位置(暂停续播)、seek 到未缓冲位置照样前进(长距离拖动响应)。
    if (!paused && !scrubbing) {
      time = projectTime(now);
      // 【起播/空洞前向吸附】播放态下 playhead 落在缓存最旧帧之前(t < cache[0].t)时前向吸附
      // 到 cache[0].t。根因：playhead 起始为 0,而帧时间戳 t 可能从非零基准开始(或起播瞬间
      // 缓存尚未覆盖到 0),此时 frameAt(time) 恒 null → 永不渲染、进度停在 0。前向吸附让时钟
      // 追上首个可用帧。仅播放态、仅向前(不越过 cache[0]),不干预 seek/scrub,不破坏单一真相。
      if (cache.length > 0 && time < cache[0].t) {
        time = cache[0].t;
      }
      // 普通播放按缓存末尾封顶，防止时钟越过供帧进度后形成 seek 风暴。未缓存 seek 是
      // 唯一例外：目标普通帧到达前保持目标位置，让 playhead 上报驱动高清线程解码目标帧。
      time = resolvePlaybackTime(time, loadedMaxTime, pendingSeekTarget);
      if (pendingSeekTarget !== null && frameAt(pendingSeekTarget)) {
        pendingSeekTarget = null;
      }
      // starving：播放中位置已越过已缓冲末尾且未到末尾 —— 后端供数跟不上(卡顿主因)。
      // 【方案A·纯内存】此时节流请求后端从 time 续供帧(见 backfillIfMissing)，画面保持上一帧，
      // 待帧解码 insertFrame 后 frameAt(time) 自然命中续播。进度条 bufferedRatio 用 loadedMaxTime，
      // 因此进度永不快于缓存(问题1根治)。
      starving = time >= loadedMaxTime - 1e-3 && time < duration - 1e-3;
    } else {
      starving = false;
    }
    lastTick = now;
    // 正常播放约 80ms 上报；拖动期间 latest-only 且最多约 120ms 一次，避免 GOP 重解风暴。
    if (shouldReportPlayhead(now, playheadReportAt, time, playheadReportTime, scrubbing)) {
      playheadReportAt = now;
      playheadReportTime = time;
      stream.send({ type: "playhead", timeSec: time, generation: stream.currentGeneration() });
    }
    // 拖动预览采用 latest-only：输入侧仅覆盖 pendingPreviewTime，本轮至多二分定位并构建一帧。
    // 若构建期间又有新输入，下一次 rAF 再消费最新目标，中间过期位置自然丢弃。
    if (scrubbing && pendingPreviewTime !== null) {
      const previewTime = pendingPreviewTime;
      pendingPreviewTime = null;
      time = previewTime;
      const previewFrame = frameAt(previewTime);
      if (previewFrame) renderFrameNow(previewFrame);
      // 拖动期间仅按约 120ms 上报当前 latest-only 位置，Raw Data 可跟随刷新，
      // 同时避免 pointer move 频率触发 generation/GOP 重解风暴。
    } else if (!scrubbing && !paused) {
      // 正常播放按 playhead 从缓存取应显示帧并渲染；未命中时节流回填。
      // 【暂停 bug 修复】暂停时(paused && !scrubbing)不得取帧渲染：否则后台 prefetch/play
      // 持续灌帧改变 frameAt(time) 命中帧，导致暂停画面仍被刷新。暂停时画面应冻结在当前帧。
      const frame = frameAt(time);
      if (frame && frame.t !== renderedTime) {
        const tb = performance.now();
        renderFrameNow(frame);
        statBuildMs = statBuildMs * 0.9 + (performance.now() - tb) * 0.1;
        statRenderCount++;
      } else if (!frame) {
        backfillIfMissing(time);
      }
    }
    //性能指标滑窗统计（每约 500ms 结算一次 fps / renderFps）。
    statRafCount++;
    const winMs = now - statWindowStart;
    if (winMs >= 500) {
      statFps = (statRafCount * 1000) / winMs;
      statRenderFps = (statRenderCount * 1000) / winMs;
      // [perf] 每 ~500ms 打印一帧率诊断：区分 rAF 被压制 / 供帧不足 / 单帧过贵三类瓶颈。
      // fps=rAF 循环频率(主线程健康度); renderFps=去重后实际渲染帧率(≈数据帧率);
      // buildMs=scene.renderFrame 几何构建耗时; gpuMs=GPU 绘制耗时; cache=内存帧数;
      // aheadSec=已缓冲领先量(<0 或接近 0 表示供帧跟不上); starving=供帧饿死标志。
      // 仅 Debug 模式转发（VIZ_DEBUG / --debug / ?debug=1），打包默认关闭，
      // 避免每 500ms 一次 IPC + stderr 输出影响播放。
      if (isDebugEnabled() && !paused && !scrubbing) {
        const line =
          `[perf] fps=${statFps.toFixed(1)} renderFps=${statRenderFps.toFixed(1)} ` +
        `buildMs=${statBuildMs.toFixed(2)} gpuMs=${scene.getLastRenderMs().toFixed(2)} ` +
          `cache=${cache.length} aheadSec=${(loadedMaxTime - time).toFixed(2)} ` +
          `starving=${starving} time=${time.toFixed(2)} loadedMax=${loadedMaxTime.toFixed(2)}`;
        void import("@tauri-apps/api/core")
          .then(({ invoke }) => invoke("ffi_log", { line }))
          .catch(() => console.log(line));
      }
      statWindowStart = now;
      statRafCount = 0;
      statRenderCount = 0;
    }
    advanceRaf = requestAnimationFrame(advance);
  };

  // 【关键·启动主时钟循环】advance 靠末尾自递归 rAF 持续推进，但必须在此 kick off 首帧，
  // 否则循环永不启动：time 停在 0、frameAt 从不被调用、渲染窗口空白，而 onFrame/persist
  // 独立于 advance 照常推进缓存进度 —— 正是"进度更新但画面无内容、进度恒 0"的根因。
  advanceRaf = requestAnimationFrame(advance);

  const resetPlayback = () => {
    duration = 0;
    frameCount = 0;
    time = 0;
    clearCache();
    paused = true;
    dataMode = null;
    // 【xplayer 模型】重置回填节流状态，新流从头判定。
    backfillAt = 0;
    backfillTarget = -1;
    pendingSeekTarget = null;
    // 显式打开新文件必是新流：重置代次哨兵，避免新流 generation 与上次相同(后端重启从 0 计数)时
    // onInfo 误判为"同一流"而不复位/不起播，导致换包后画面卡住不动。
    lastInfoGeneration = -1;
  };

  return {
    setPaused(value) {
      // 末尾再次点播放先回绕到起点（对齐 bug1 修复）。
      if (value === false && duration > 0 && time >= duration - 1e-3) {
        time = 0;
        clearCache();
        stream.seek(0);
      }
      paused = value;
      rawDataWiring.setPaused(value);
      lastTick = performance.now();
      // 【xplayer 单一真相模型】暂停/恢复只切 paused 标志并重置 lastTick，绝不改 time(playhead)。
      // 由此暂停冻结的位置在恢复时天然续播 —— advance 只用 time 取帧，回填从不改 time。
      // 暂停只冻结前端媒体时钟和画面；后端继续顺序供帧,以便内存缓存持续增长到 100%。
    },
    setSpeed(value) {
      speed = value;
      stream.send({ type: "setSpeed", speed: value });
    },
    setScrubbing(active) {
      scrubbing = active;
      if (!active) {
        pendingPreviewTime = null;
      }
      lastTick = performance.now();
    },
    requestPreview(value) {
      if (!scrubbing) return;
      // 生产者只覆盖最新目标；真正二分定位和场景构建由 advance 的单消费者完成。
      pendingPreviewTime = Math.max(0, Math.min(duration, value));
    },
    seek(value) {
      // 正式 seek 只在拖动释放/键盘跳转时执行：立即定位内存帧，未覆盖则节流续供。
      const target = Math.max(0, Math.min(duration, value));
      time = target;
      pendingPreviewTime = null;
      lastTick = performance.now();
      // pointer-up 先结束 scrubbing、再调用 seek；因此必须在这里上报最终目标，
      // 不能在 setScrubbing(false) 中上报尚未提交的旧预览位置。
      playheadReportAt = lastTick;
      playheadReportTime = target;
      stream.send({ type: "playhead", timeSec: target, generation: stream.currentGeneration() });
      const memoryFrame = frameAt(target);
      const aheadFrame = frameAt(target + 3);
      const coveredAhead = aheadFrame !== null && aheadFrame.t > target + 0.5;
      if (memoryFrame) {
        pendingSeekTarget = null;
        renderFrameNow(memoryFrame);
        if (!coveredAhead) backfillIfMissing(target);
        return;
      }
      pendingSeekTarget = target;
      backfillIfMissing(target);
    },
    setLayerVisible(layerId, visible) {
      scene.setLayerVisible(layerId, visible);
      stream.send({ type: "setLayerVisible", layerId, visible });
    },
    subscribeImage(channel, enabled) {
      stream.subscribeImage(channel, enabled);
    },
    subscribePointCloud(channel, enabled) {
      stream.subscribePointCloud(channel, enabled);
    },
    subscribeRawData(channel, enabled) {
      // 先驱动后端订阅（type-11 raw 帧下发/停发），再更新 wiring 选择态；
      // wiring 换通道会清旧面板内容、关闭时停 decode（订阅默认关闭，零成本）。
      rawDataSubscription.update(channel, enabled);
      rawDataWiring.subscribe(channel, enabled);
      if (enabled) {
        // 新通道可能没有本地缓存；立即请求当前 playhead，不能依赖下一帧自然到达。
        playheadReportAt = performance.now();
        playheadReportTime = time;
        stream.send({ type: "playhead", timeSec: time, generation: stream.currentGeneration() });
      }
    },
    setCameraMode(mode) {
      scene.setCameraMode(mode);
    },
    getCameraMode: () => scene.getCameraMode(),
    // 大数据当前帧读取：交给持有者返回该 channel 的当前帧（未订阅/未到达则 undefined）。
    getCurrentBigData: (channel: string) => bigDataStore.get(channel),
    // RawData 解码面板当前 state（定义/选择/解码结果/查询高亮）。未选/未解码返回初始态。
    getRawDataPanelState: () => rawDataWiring.getState(),
    // RawData 面板 UI 交互写入口（搜索/导航/视口）；类型收窄，UI 无法绕过链路改内部态。
    dispatchRawDataPanelAction: (action) => rawDataWiring.dispatchUi(action),
    setRawDataPanelVisible: (visible) => rawDataWiring.setVisible(visible),
    // 【缩略图】进度条拖动预览：查某 channel 最接近 tSec 的缩略图 blobUrl（未生成返回 null）。
    getThumbnailEntry: (channel: string, tSec: number) => thumbnailStore.findNearestEntry(channel, tSec),
    getThumbnailUrl: (channel: string, tSec: number) => thumbnailStore.findNearestEntry(channel, tSec)?.blobUrl ?? null,
    listFiles() {
      stream.listFiles();
    },
    openFile(fileName) {
      resetPlayback();
      opts.onStatus?.(`正在加载 ${fileName}`);
      stream.open(fileName);
    },
    openRecord(record) {
      resetPlayback();
      opts.onStatus?.(`正在查询数据包 ${record} 的 S3 路径`);
      stream.openRecord(record);
    },
    openByName(fileName) {
      resetPlayback();
      opts.onStatus?.(`正在打开 ${fileName}（本地缓存优先，未命中走 S3 流式）`);
      stream.openByName(fileName);
    },
    async uploadFile(file, onProgress) {
      resetPlayback();
      opts.onStatus?.(`正在上传 ${file.name}`);
      await stream.upload(file, onProgress);
      opts.onStatus?.(`已上传 ${file.name}，正在解码`);
    },
    // 【xplayer 单一真相模型】UI 时间取 playhead(time) —— 位置的唯一权威源。
    // 不再取 renderedTime：暂停恢复后进度条须停在暂停位(playhead)，而非回填途中最近渲染帧，
    // 否则表现为"从缓存最近进度重播"。回填从不改 time，故 time 始终等于用户当前所在位置。
    getTime: () => time,
    // 【方案A·纯内存】缓存/可拖动进度统一取 loadedMaxTime(后端已推送并解码进内存的最大帧
    // 时间戳)。不再有 IndexedDB 落盘进度 persistedMaxTime —— 进度永不快于内存缓存(问题1根治)。
    getLoadedTime: () => loadedMaxTime,
    // 【方案A·纯内存】可 seek 上界 = 已缓存内存最大时刻。seek 到未缓存区时由 seek() 向后端
    // 请求续供帧，故这里给内存前沿即可(拖动范围随缓存增长而放开)。
    getSeekableMax: () => loadedMaxTime,
    getDuration: () => duration,
    getFrameCount: () => frameCount,
    isPaused: () => paused,
    getSpeed: () => speed,
    getCharts: () => lastCharts,
    getDataMode: () => dataMode,
    getStats: () => ({
      fps: Math.round(statFps * 10) / 10,
      renderFps: Math.round(statRenderFps * 10) / 10,
      cacheFrames: cache.length,
      cacheBytesMB: Math.round(cacheBytes / 1_000_000),
      bufferAheadSec: Math.max(0, loadedMaxTime - time),
      starving,
      frameBuildMs: Math.round(statBuildMs * 100) / 100,
      gpuRenderMs: Math.round(scene.getLastRenderMs() * 100) / 100,
      bufferedRatio: duration > 0 ? Math.min(1, loadedMaxTime / duration) : 0,
    }),
    dispose() {
      disposed = true;
      // 取消主时钟 rAF：否则 advance 仍被反复入队,即使首行 if(disposed) return,
      // 也会持续占用主线程、阻止浏览器进入 idle,并保留 stream/scene 等闭包引用
      // 阻碍 GC。必须在 close 之前 cancel,避免 dispose 后再触一次 advance。
      if (advanceRaf) cancelAnimationFrame(advanceRaf);
      stream.close();
      decoderPool.dispose();
      // RawData decoder Worker terminate + reject pending（错误隔离，不影响其余 dispose）。
      rawDataWiring.dispose();
      scene.dispose();
    },
  };
}
