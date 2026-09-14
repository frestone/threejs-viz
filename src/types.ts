// 图层定义与引擎接口类型。图层 id 对齐 configs/decoder.json 的 layers 与
// configs/demo.layers.json 的 scene layers（localization/planning/perception 等）。

// 图层分组/展示信息由后端随 LAYER_DEFS 下发（decoder*.json 的 layers[].group/
// groupLabel/label），前端不再硬编码图层树，唯一来源为后端配置。

// 图表类型（对齐 viz-core/proto/frame.proto 的 ChartData/ChartSeries）。
export type ChartSeriesKind = "line" | "scatter" | "band_upper" | "band_lower";

export interface ChartSeriesData {
  name: string;
  kind: ChartSeriesKind;
  color: string;
  x: number[];
  y: number[];
}

export interface ChartData {
  id: string;
  title: string;
  xLabel: string;
  yLabel: string;
  series: ChartSeriesData[];
}

// 图表配置定义(对齐 configs/decoder.json 的 charts 项)。用于控制面板持久
// 展示显隐按钮:与是否有帧数据无关,配置几个图表就渲染几个控制按钮。
// 前端不硬编码图表列表——定义由服务端连接建立时下发(唯一来源 decoder.json)。
export interface ChartDef {
  id: string;
  title: string;
  // 初始是否可见(对齐 decoder.json chart.visible)。
  visible: boolean;
}

// 图层样式定义（对齐后端 LayerConfig/LayerStyle，随连接下发 LAYER_DEFS）。
// 前端渲染配色不再硬编码，唯一来源为后端（decoder.json 顶层 layers 或内建 defaults）。
export interface LayerStyleDef {
  // RGBA 颜色分量，范围 [0,1]，可直接用于 THREE.Color(r,g,b)。
  color: { r: number; g: number; b: number; a: number };
  opacity: number;
  width: number; // 线宽 / ribbon 宽
  height: number; // 离地抬升
  depthBias: number;
  colorByType: boolean; // 障碍物按 type 着色
}

export interface LayerDef {
  id: string;
  source: string; // 对齐 Frame.layers 的 key（localization/trajectory/path/perception）
  draw: string; // line/ribbon/points/box/polygon
  visible: boolean;
  style: LayerStyleDef;
  // 以下三项由后端随 LAYER_DEFS 下发（decoder*.json layers[].group/groupLabel/label），
  // 供前端动态构建图层面板分组树——唯一配置来源，前端不再硬编码 layer_tree.json。
  group?: string;       // 分组 id（ego/planning/perception/prediction/map）
  groupLabel?: string;  // 分组展示名（中文）
  label?: string;       // 图层展示名（中文，回退 id）
}

// 相机图像通道定义：由后端随IMAGE_DEFS(type=8) 下发（decoder*.json imageChannels），
// 供前端在图像组动态生成 checkbox。默认不勾选、不订阅（零成本），勾选后才 subscribeImage。
export interface ImageChannelDef {
  id: string; // = Frame.images 的 key
  topic: string; // MCAP topic
  label: string; // UI 展示名（后端已保证非空，回退 id）
}

// RawData 通道定义：由后端随 type 12(RAW_DATA_DEFS) 下发（spec §6 rawData[]）。
// 与 ImageChannelDef 同族，但携带 available 及其条件字段：
// available=true 时带 messageType + schemaId(注意 schemaId 是 string，后端 std::to_string 转换)；
// available=false 时带 unavailableReason（可展示原因，不携带无效引用）。
// schemaId 引用 RawDataSchemaDef.id（同为 string）。
export interface RawDataChannelDef {
  id: string; // 通道稳定 id（配置项 id）
  topic: string; // MCAP topic
  label: string; // UI 展示名（后端已保证非空，回退 id）
  available: boolean; // 是否可解码/可用
  unavailableReason?: string; // 仅 available=false 时出现
  messageType?: string; // 仅 available=true 时出现（全限定 message type）
  schemaId?: string; // 仅 available=true 时出现；引用 schemas[].id（string）
}

// RawData schema 定义：protobuf FileDescriptorSet 的 base64 透传（spec §6 schemas[]）。
// 本任务(Task 4)仅解析+透传 dataBase64，不 decode base64、不 build descriptor（Task 5/6 负责）。
export interface RawDataSchemaDef {
  id: string; // schema 稳定 id（后端 std::to_string，string）
  encoding: "protobuf"; // 目前仅支持 protobuf
  dataBase64: string; // FileDescriptorSet 的 base64 原样字符串
}

// type 12 RawData Definitions 快照。外层仅 rawData+schemas，
// 【不含 generation】——代次由 type 11 帧头承担，勿在此引入 generation。
export interface RawDataDefs {
  rawData: RawDataChannelDef[];
  schemas: RawDataSchemaDef[];
}

// 相机模式：follow=跟随主车（默认相机锁定 ego）；free=自由浏览（俯视视角、鼠标可拖拽平移浏览）。
export type CameraMode = "follow" | "free";
// 数据包运行模式：highprec=高精，lightmap=轻图；null=尚未加载/未知。
export type DataMode = "highprec" | "lightmap" | null;

// 播放性能指标（供帧率组件分析播放卡顿）：
// - fps：rAF 渲染循环的实际帧率（浏览器每秒调用 advance 的次数）。
// - renderFps：实际调用 scene.renderFrame 的频率（去重后，等于数据帧刷新率，理想 ~10Hz）。
// - cacheFrames：当前浏览器帧缓存中的帧数。
// - bufferAheadSec：已加载最大帧时间戳 loadedMaxTime 与当前播放 time 的差值；
//   越接近 0 说明后端供数跟不上、播放被封顶等待（卡顿主因之一）。
// - dropped：因 time 被 loadedMaxTime 封顶而本应推进却未推进的累计秒数近似（供数不足信号）。
export interface PlaybackStats {
  fps: number;
  renderFps: number;
  cacheFrames: number;
  // 当前帧缓存占用的估算内存（MB）。按字节数背压的直接可观测指标，用于确认内存被夹在阈值内。
  cacheBytesMB: number;
  bufferAheadSec: number;
  starving: boolean;
  // 单帧耗时诊断（毫秒）：区分瓶颈在几何重建侧还是 GPU 绘制侧。
  frameBuildMs: number;  // scene.renderFrame（几何 build）平均耗时
  gpuRenderMs: number;   // renderer.render（GPU 绘制）平均耗时
  // 缓存进度：已缓冲到的媒体时间 / 总时长（0~1），用于进度条下方缓存进度显示。
  bufferedRatio: number;
}

// 引擎对外暴露的最小控制面。
export interface EngineApi {
  // 播放控制
  setPaused(paused: boolean): void;
  setSpeed(speed: number): void;
  seek(timeSec: number): void;
  // 拖动预览只更新最新目标，由引擎 rAF 每轮至多消费并渲染一帧；不排队、不请求后端。
  // 释放后再由 seek 正式提交目标，必要时触发后端续供。
  requestPreview?(timeSec: number): void;
  setScrubbing?(active: boolean): void;
  setLayerVisible(layerId: string, visible: boolean): void;
  // 订阅/取消某相机图像通道（如 camera360_front）。开启后服务端会在发帧前
  // 解码 HEVC 并把 JPEG 填入 frame.images，前端经 onImage 回调拿到并显示。
  subscribeImage?(channel: string, enabled: boolean): void;
  // 订阅/取消某点云通道（如 lidar_top）。开启后服务端在发帧/按段注入时把点云填入
  // frame.point_clouds，前端经 pointClouds 渲染；取消则不再下发，缓存段按需回填。
  subscribePointCloud?(channel: string, enabled: boolean): void;
  // 订阅/取消某原始数据通道（RawData：原始传感器字节流等）。开启后服务端把原始字节
  // 透传填入 frame.raw_data，前端按 format 语义自行解析；默认不订阅（零成本）。
  subscribeRawData?(channel: string, enabled: boolean): void;
  // RawData 面板折叠时暂停前端文本解码，展开后恢复当前缓存帧解码。
  setRawDataPanelVisible?(visible: boolean): void;
  listFiles?(): void;
  openFile?(fileName: string): void;
  // 输入 MCAP 数据包 record 名，经元数据服务查询 S3 路径后流式打开。
  openRecord?(record: string): void;
  // 统一打开入口：只传文件名——服务端本地缓存命中走本地 reader，未命中走 S3 流式。
  openByName?(fileName: string): void;
  uploadFile?(file: File, onProgress: (ratio: number) => void): Promise<void>;
  // 状态读取（UI 轮询）
  getTime(): number;
  // 已加载(缓存)到的最大帧时间戳。进度条据此限制其平滑动画不越过已加载边界，
  // 避免后端回填未跟上(starving)时进度条越过实际能渲染到的画面。未实现时上层退化为 duration。
  getLoadedTime?(): number;
  // 可拖动上界：已落盘到 IndexedDB 的最大帧时间戳。seek 只消费已缓存数据，
  // 故进度条拖动范围硬限制在 [0, getSeekableMax()]；未实现时上层退化为不限制。
  getSeekableMax?(): number;
  getDuration(): number;
  getFrameCount(): number;
  isPaused(): boolean;
  getSpeed(): number;
  // 当前帧图表数据（three 引擎实现；stream/mock 返回空数组）。
  getCharts?(): ChartData[];
  // 数据包运行模式（highprec/lightmap），由服务端 StreamInfo 下发；未加载时为 null。
  getDataMode?(): DataMode;
  // 播放性能指标（帧率/缓存深度/供数落后），供帧率组件分析卡顿；仅 three 引擎实现。
  getStats?(): PlaybackStats;
  // 相机模式：follow=跟随主车（默认）；free=自由浏览（俯视、可拖拽平移）。
  setCameraMode?(mode: CameraMode): void;
  getCameraMode?(): CameraMode;
  // 大数据（图像/RawData）当前帧读取：大数据不进全量缓存，由引擎按 playhead 实时持有
  // 每 channel 的当前帧。上层在 onBigDataUpdate 回调后调用此方法取最新字节渲染。
  getCurrentBigData?(channel: string): {
    tSec: number;
    gen: number;
    kind: "image" | "raw";
    // 原始消息 header.seq(图像诊断序号；raw 为 0)，供面板叠加显示。
    seq: number;
    payload: Uint8Array;
    blobUrl?: string;
  } | undefined;
  // 【缩略图预览】进度条拖动时按最接近 tSec 查完整低清缩略图条目，
  // 使图片、时间与源图像序号保持同源。未生成区域返回 null；blobUrl 生命周期由引擎管理。
  getThumbnailEntry?(channel: string, tSec: number): {
    tSec: number;
    blobUrl: string;
    seq: number;
  } | null;
  // 兼容仅需图片 URL 的调用方。
  getThumbnailUrl?(channel: string, tSec: number): string | null;
  // 【RawData 面板】当前 RawData 解码面板 state（通道定义/选中/解码文本行/查询高亮/视口）。
  // 由引擎在 onRawDataPanelUpdate 回调后驱动 UI 拉取；未选/未解码时为初始态。
  getRawDataPanelState?(): import("./rawDataPanelState").PanelState;
  // 【RawData 面板】UI 交互写入口（搜索/导航/视口）；类型收窄为 UI action 子集，
  // 组件无法绕过解码链路直接改内部 state（单一真相源仍由 panelReducer 演进）。
  dispatchRawDataPanelAction?(action: import("./engine/rawDataWiring").RawDataUiAction): void;
  // 生命周期
  dispose(): void;
}