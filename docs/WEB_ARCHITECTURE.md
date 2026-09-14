# Web 统一架构设计 — Three.js 前端渲染 + React 壳 + 厚后端推流

> 目标：Web 与桌面**共用同一份 React 应用**，前端渲染核心用纯 JavaScript 的
> **Three.js**（WebGL2）驱动，桌面端用 **Tauri** 装载同一份构建。后端（厚后端
> mcap 解码 + WebSocket Protobuf Frame 推流）**保持不变**。
>
> **重构说明**：本文档由原「Emscripten WASM + Filament」架构重写而来。前端不再
> 编译 C++/WASM、不再需要 embind 绑定与 Filament WebGL 后端；改为 npm 生态的
> Three.js + protobufjs，构建与迭代显著简化，产物体积大幅下降（无 3.1MB WASM 包，
> JS 产物 gzip 约 553 kB）。

---

## 1. 现状盘点（重构基础）

| 现状 | 说明 | 对 Three.js 版的意义 |
| --- | --- | --- |
| 后端厚解码链路 | `server.cpp` + S3/MCAP 流式解码 + WebSocket Protobuf Frame 推流 | **完全保留**，前端只换渲染核心 |
| `frame.proto` 协议 | Frame = layers map + ego_anchor/ego_yaw/ego_valid + charts | 前端用 protobufjs 运行时加载解析，零代码生成 |
| React UI | App / PlaybackBar / LayerPanel / ChartPanel + echarts | **零改动复用**，仅改引擎接线与文案 |
| `FrameStream.ts` | WebSocket 二进制协议层（msgType + frameSeq + Frame bytes） | 零改动复用 |
| Tauri 外壳 | `src-tauri/`（tauri-plugin-dialog 本地文件对话框） | 迁移改名（Three.js Viz），零逻辑改动 |
| 渲染层（原 WASM/Filament） | embind 绑定 + Filament WebGL 后端 | **移除**，改为 `threeEngine.ts`（Three.js） |

**结论**：后端与协议、React UI、WebSocket 层、Tauri 壳全部保留，重构聚焦于把
「WASM+Filament 渲染核心」替换为「protobufjs 解码 + Three.js 渲染」。

---

## 2. 目标架构（Web/桌面共用 React + Three.js）

```
                ┌───────────────────────────────────────────┐
                │       厚后端解码服务 (C++ viz-core native)     │
                │  S3/本地 MCAP 流式 → 按时间窗解码 → 组装 Frame │
                │  → Protobuf 序列化 → WebSocket 推送           │
                └──────────────────┬────────────────────────┘
                                   │ WebSocket
                          [msgType | frameSeq | Protobuf Frame bytes]
                                   │
                ┌──────────────────┴────────────────────────┐
                │            前端 (React + Vite + TS)          │
                │  protobufjs 解码 Frame                       │
                │  → Three.js Scene/Camera/WebGLRenderer 渲染   │
                │  <canvas> + 播放/图层/图表/文件面板            │
                └──────────────────┬────────────────────────┘
                    ┌──────────────┴───────────────┐
              ┌─────┴────────┐              ┌───────┴─────────┐
              │  浏览器 (Web)  │              │  Tauri 桌面外壳   │
              │  静态托管 dist │              │  同一 React 构建  │
              └──────────────┘              └─────────────────┘
```

构建产物：
1. **React 应用**（Vite）：Web 与桌面**同一份构建产物**（`dist/`）。
2. **Web 部署**：静态托管（nginx/CDN）。
3. **桌面 App**：Tauri 打包同一 React 构建为原生可执行（Win/macOS/Linux）。

**为何选 Tauri（而非 Electron）**：包体小（用系统 WebView，不捆绑 Chromium）、
启动快、Rust 壳安全性好；本项目只需「装一个 Web 应用 + 本地文件访问」。

---

## 2.5 数据来源、部署形态与解码位置

> **架构决策**：mcap 解码与 Frame 组装放在**后端**，前端只接收**最小化的结构化
> Protobuf Frame**。网络上只走「渲染所需的几何数据」，前端不做 zstd 解压/mcap 解析。

### 2.5.1 数据来源与部署形态

| 部署形态 | 支持的数据来源 | 说明 |
| --- | --- | --- |
| **本地部署**（桌面 Tauri / 本机浏览器） | ① 本地文件 ② 输入文件名 → S3 流式 | 本地可直接选 mcap；也可只输文件名，元数据接口查 bucket/key（本地缓存命中则直读）后流式读 |
| **远程服务器部署**（云端 Web） | 仅：输入文件名 → S3 流式 | 无本地 FS，输入文件名，元数据接口查 bucket/key 后 AWS SDK GetObject 流式读 |

**注**：无论哪种来源，mcap 字节都进入**后端解码器**，不直达前端。

### 2.5.2 厚后端解码 + Frame 序列化推送链路

```
用户输入文件名 / 选本地文件
      │
      ▼
[后端] 输文件名 → 元数据 HTTP 接口查 bucket/key（或直接本地路径）
      │  先查本地缓存目录命中则直读，否则走 S3
      ▼
[后端 viz-core(native)] S3 流式(AWS SDK GetObject + HTTP Range,按 Summary/ChunkIndex 定位)
      │  按当前时间窗惰性解码 + 组装 Frame(map<string,LayerData>)
      │  ★ 绝不整文件入内存,只解码时间窗所需 chunk
      ▼
[序列化] Frame → Protobuf 紧凑二进制(几何语义:position/points/size/heading/text + id/type/score)
      │  WebSocket 流式推送(随播放推进按需推帧)
      ▼
[前端] protobufjs 解码 Frame → Three.js buildGeometry → WebGL 渲染
```

**关键设计点**：

- **后端厚（解码+组装）**：后端复用 **viz-core(native)** 从 S3/本地流式读 mcap、
  按时间窗惰性解码、组装成结构化 `Frame`，**不做渲染**。
- **传输最小化**：网络上只传 Frame 的 Protobuf 紧凑序列化（几何语义字段），
  **不传原始 mcap 字节、不传 zstd 压缩块**。
- **前端只解码不解压**：前端用 protobufjs 反序列化 Frame，无 mcap reader / zstd。
- **仍遵循按需/流式约束**：后端按当前回放时间窗解码并推送 Frame；前端只缓存
  滑动窗口内的 Frame（LRU 释放）。
- **协议**：控制指令（seek/play/speed/layer 开关）前端→后端；Frame 数据流
  后端→前端，走 **WebSocket**（二进制帧）。
- **S3 访问机制**：后端按文件名先查元数据 HTTP 接口得 bucket/key，再用 AWS SDK
  C++ `S3Client`（服务端凭证）`GetObject` 流式读；本地缓存命中则直读；前端不
  直连 S3，天然规避跨域与凭证暴露。

---

## 3. 前端技术栈与依赖

- **Vite + React + TypeScript**（无 Emscripten/Bazel，纯 npm 工具链）。
- **three**（+ `@types/three`）：核心渲染库，`OrbitControls` 来自
  `three/examples/jsm/controls/OrbitControls`。
- **protobufjs**：运行时 `protobuf.load("/frame.proto")` + `lookupType("viz.Frame")`
  → `decode`/`toObject`（`enums:Number, longs:Number`），免代码生成。
- **echarts**：图表面板（浮动可拖拽 ChartPanel）。
- **@tauri-apps/api** + **@tauri-apps/plugin-dialog**：桌面原生文件对话框。

> 相比原架构，**移除**了 emsdk、Filament WASM 发行包、embind 绑定层、WASM 版
> zstd/matc 等一整套重型工具链依赖。

---

## 4. WebSocket 帧协议与 Frame 解码

### 4.1 WebSocket 二进制帧格式

每个 WebSocket 二进制消息以 1 字节 `msgType` 开头，据此分派：

```
msgType = 1 (FRAME)       [u8=1][u64 frameSeq (LE)] + Protobuf Frame bytes  ← FRAME_HEADER_BYTES=9
msgType = 2 (STREAM_INFO) [u8=2] + JSON { durationSec, frameCount, generation, dataMode }
msgType = 3 (FILE_LIST)   [u8=3] + JSON { files:[{name,sizeBytes}] }
msgType = 4 (ERROR)       [u8=4] + JSON { message }
msgType = 5 (UPLOAD)      [u8=5] + JSON { state: ready|processing|complete }
msgType = 6 (CHART_DEFS)  [u8=6] + JSON { charts:[ChartDef] }        ← 连接即下发
msgType = 7 (LAYER_DEFS)  [u8=7] + JSON { layers:[LayerDef] }        ← 连接即下发(图层分组/中文名单一来源)
msgType = 8 (IMAGE_DEFS)  [u8=8] + JSON { images:[ImageChannelDef] } ← 连接即下发(相机通道清单)
```

- 只有 `FRAME`(1) 是二进制 Protobuf，其余均为 `[msgType][JSON]` 文本载荷。
- `frameSeq`：帧序号，用于丢帧检测与 seek 后丢弃过期帧。
- `generation`（见 STREAM_INFO / seek / requestRange）：**代次**。seek 自增代次；分段
  预取 `requestRange` 复用当前代次，使预取帧被前端归并入段缓存而非丢弃。

### 4.1.1 上行控制指令（前端 → 后端，均为 JSON）

`FrameStream.ts` 的 `ControlMsg` union：

| type | 语义 |
| --- | --- |
| `listFiles` | 列本地缓存目录可用 MCAP |
| `open` | 打开数据源（`source: local\|s3\|auto`，本地/元数据查 S3/自动） |
| `uploadBegin/uploadComplete/uploadCancel` | 分块上传本地文件（1 MB/块 + bufferedAmount 背压） |
| `seek` | 跳转（自增 `generation`） |
| `requestRange` | **分段预取**：按 `[startSec,endSec]` 全速回填（复用 `generation`），不影响播放游标 |
| `setPaused` / `setSpeed` | 暂停 / 倍速 |
| `setLayerVisible` | 图层显隐 |
| `subscribeImage` / `subscribePointCloud` / `subscribeRawData` | **按需订阅**：勾选后才让后端在发帧前解码/注入对应通道数据 |

### 4.2 Frame 解码（`frameCodec.ts`）

```ts
// keepCase:true 必需——proto 字段为 snake_case(ego_valid/ego_anchor/point_clouds)，
// 若被转成 camelCase，obj.ego_valid 恒 undefined → egoValid 恒 false → 相机跟随失效。
const root = protobuf.parse(text, { keepCase: true }).root;
const FrameType = root.lookupType("viz.Frame");
const frame = FrameType.decode(bytes);
const obj = FrameType.toObject(frame, { defaults: true, enums: Number, longs: Number });
```

`public/frame.proto` 为协议副本（与后端 `viz-core/proto/frame.proto` 唯一真源对齐）：

```proto
syntax = "proto3";
package viz;

message Vec3 { float x = 1; float y = 2; float z = 3; }

// 7 种几何图元（前端 GEOMETRY_KINDS 按枚举下标映射）
enum GeometryKind {
  POINT = 0; LINESTRIP = 1; BOX = 2; POLYGON = 3; TEXT = 4;
  ARROW = 5;                 // position + heading + size(缩放)
  PLANNING_TRAJECTORY = 6;   // points 折线沿法向扩展成带状多边形，宽度见 style.width
}

message GeometryItem {
  Vec3 position = 1; repeated Vec3 points = 2; Vec3 size = 3;
  float heading = 4; string text = 5;
  int32 id = 6; int32 type = 7; float score = 8;   // type 供 colorByType 上色
}
message LayerData { GeometryKind kind = 1; repeated GeometryItem items = 2; }

message Image      { string format=1; uint32 width=2; uint32 height=3; bytes data=4; double t=5; }
message PointCloud { repeated float xyz=1[packed]; repeated float rgb=2[packed];
                     repeated float intensity=3[packed]; double t=4; string frame_id=5; string encoding=6; }
message RawData    { string topic=1; string format=2; bytes data=3;
                     double t=4; string frame_id=5; string encoding=6; uint64 seq=7; }

message Frame {
  double t = 1;
  map<string, LayerData> layers = 2;
  Vec3 ego_anchor = 3; float ego_yaw = 4; bool ego_valid = 5;
  map<string, ChartData>  charts = 6;
  map<string, Image>      images = 7;        // 按相机通道名索引，勾选订阅后注入
  map<string, PointCloud> point_clouds = 8;  // 按点云图层名索引，勾选订阅后注入
  map<string, RawData>    raw_data = 9;       // 按 RawData 图层名索引，勾选订阅后注入
}
```

> `frame.proto` 字段须与后端 **严格一一对齐**，为唯一真源；改结构须两端同步。
> `images/point_clouds/raw_data` 默认不下发，仅前端勾选订阅（见新增章节「按需订阅」）后按需注入。

---

## 5. Three.js 渲染引擎（核心新增，`threeEngine.ts`）

替代原 embind + Filament WebGL 后端，纯 TS 实现。

### 5.1 初始化

- `WebGLRenderer` 绑定到 `<canvas>`；`Scene` + `PerspectiveCamera`。
- `OrbitControls`（`three/examples/jsm`）接管鼠标 orbit/dolly/pan，**App 层不再手写鼠标事件**。
- `ResizeObserver` 同步 canvas 尺寸与 `devicePixelRatio`。
- `GridHelper` 地面参考网格。
- **坐标系变换**：`frame.proto` 为右手系（x 前/y 左/z 上），Three.js 默认 y 上；
  用一个 world group 绕 X 轴旋转 `-90°`，使 z-up 数据在 y-up 场景中正确显示。

### 5.2 每帧 `renderFrame(frame)`（对齐原 `renderer.cpp` buildGeometry 语义）

按 `frame.layers`（map）遍历，依 `LayerData.kind` 构建 Three.js 对象：

| kind | Three.js 映射 |
| --- | --- |
| `POINT` | 小 box 标记 |
| `LINESTRIP` | `THREE.Line`（points 数组） |
| `BOX` | 中心抬 `size.z/2` + 绕 Z 轴 `heading` 旋转 + 棱线（`EdgesGeometry`/`LineSegments`）+ 半透明面（opacity 0.25） |
| `POLYGON` | 闭合 `THREE.Line`（首尾相连的闭合环） |
| `TEXT` | canvas 纹理 `Sprite`（position + text） |
| `ARROW` | 箭头（position + heading + size 缩放） |
| `PLANNING_TRAJECTORY` | points 折线沿法向扩展 `style.width` 成带状多边形（规划轨迹带） |

- `type` 字段按类型上色（对齐原 `colorByType`）；类型名/颜色单一来源为
  `public/obstacle_colors.json`（`perceptionTypeName/perceptionTypeColorHex`），不硬编码。
- **自车相机跟随**：用 `ego_anchor` / `ego_yaw`（`applyMatrix4(world.matrixWorld)`）。
- **图层显隐**：`setLayerVisible(layerId, visible)` 切换对应对象 `visible`。
- **资源回收**：每帧 clear+rebuild 并 dispose 旧 geometry/material（GPU 释放）。

### 5.2.1 点云渲染（`renderPointClouds`）

- 每个点云通道对应一个持久化 `THREE.Points`（`BufferGeometry` + `PointsMaterial`），
  跨帧复用对象，仅更新 `position` 属性（点数不变时 `set()`，变化时重建 attribute）。
- `pc.rgb` 存在且逐点对齐时启用 `vertexColors`；否则删除 color 属性用纯色。
- 本帧未出现的通道：`setDrawRange(0,0)` 清空绘制但保留对象，避免频繁重建。
- 点云 `xyz/rgb/intensity` 为 packed `Float32Array`，解码在 Worker 内完成并零拷贝
  transfer 回主线程（见「解码 Worker 池」）。

### 5.2.2 转弯时间补偿（`egoPoseAt`）

轻图（lightmap）下障碍物为车体相对坐标，若统一用**当前帧** ego 位姿变换，自车转弯时
整簇 box 会偏航。修复：按障碍物采样时刻用 `egoPoseAt` 插值出对应时刻的 ego 位姿再变换。

### 5.3 引擎级联入口（`engine.ts`）

`EngineKind = "three" | "stream" | "mock"`，`App.tsx` 用 try/catch 逐级回退：

```
createEngine("three")  ── 失败 ─▶ ("stream")  ── 失败 ─▶ ("mock")
```

> 当前 `stream` 档位在 `engine.ts` 简化退回 mock；App 层已实现真正 try/catch 级联，
> 真实推流渲染接入为后续项。

## 5.4 解码 Worker 池（`frameDecoder.ts` + `decodeWorker.ts`）

主线程单线程解码 Protobuf 在高帧率下成为瓶颈（~23 Hz）。引入 Worker 池后突破：

- `FrameDecoderPool`：创建 `N = max(1, min(cpu/2, 4))` 个 module Worker，`decode(seq,bytes)`
  以 round-robin 分发，`postMessage` 时 transfer `bytes.buffer` 零拷贝送入 Worker。
- `decodeWorker.ts`：`loadFrameCodec` 就绪前 pending 排队；就绪后 `decodeFrame` +
  `collectTransferables` 收集点云/图像 `ArrayBuffer`，`postMessage(resp, transfer)`
  零拷贝回传主线程，避免大数组结构化克隆。
- 结果按 `id` 与主线程的 pending Promise 配对；`dispose` 全部 terminate 并 reject。

## 5.5 分段缓存与滑动窗口预取（`threeEngine.ts`）

前端播放采用「浏览器缓存帧序列 + 本地 rAF 时钟主导」，配合按段预取：

- `SEGMENT_SEC = 1`：按 1 秒切段，`segIndexOf(t) = floor(t)`。
- `requestedSegments: Set<number>`：记录已向后端 `requestRange` 过（含在途）的段，避免重复请求。
- `ensureRange(from,to)`：把窗口内**未缓存且未在途**的连续缺段合并成一次 `requestRange`
  下发（`flush` 归并连续 run），减少往返。
- `PREFETCH_AHEAD_SEGMENTS = 5`：播放/seek 时向前预取 5 段（滑动窗口深度）。
- `segmentBuffered(seg)`：判定某段是否已在内存缓存命中。
- 缓存按**字节**而非帧数背压（`estimateFrameBytes`）：一帧几十万点云与空帧差数量级，
  按字节才能真正夹住内存；换包/清缓存时 `requestedSegments` 一并作废。

## 5.6 按需订阅与回填（`subscribe* + refetchAroundCurrent`）

图像/点云/RawData 不在默认传输通道，勾选后才订阅拉取：

- `subscribeImage / subscribePointCloud / subscribeRawData(channel, enabled)`：三对等接口，
  转调 `stream.subscribe*` 通知后端在发帧前解码/注入对应通道数据。
- 订阅状态切换后调 `refetchAroundCurrent()`：清当前预取窗口的 `requestedSegments` 记账，
  再 `ensureRange` 回填，使已缓存段带上新订阅的数据重新拉取。
- RawData 原样透传字节（`topic/format/data/t/frame_id/encoding/seq`）；点云本阶段占位
  透传（`encoding="raw_msg"`，尚未解析 packed float）。

## 5.7 本地时钟主导播放（rAF advance）

- `advance()`（每 rAF）：播放时按 `speed` 推进 `time`，但**封顶** `loadedMaxTime`
  （缓存已加载的最大帧时间戳），后端供数跟不上则 `starving=true`（卡顿主因）。
- `projectTime(now)`：`advance` 与 `getTime` 共用同一推进逻辑，保证进度条与渲染一致；
  进度条读 `projectTime` 而非 `time`，避免 rAF 被重量级 `renderFrame` 拖慢时一顿一顿。
- `getStats()` 暴露 `statFps`（rAF 帧率）、`bufferAheadSec`、`bufferedRatio` 等供 HUD。

---

## 6. React 前端壳（复用 + 适配）

React UI **零业务改动**迁移，仅改引擎接线与文案：

```
src/
├── main.tsx / App.tsx        # 入口 + 主界面（引擎级联/播放/图层/图表/文件打开）
├── components/
│   ├── PlaybackBar.tsx       # 播放/暂停/seek/倍速
│   ├── LayerPanel.tsx        # 图层树三态勾选（all/none/partial）
│   └── ChartPanel.tsx        # echarts 浮动可拖拽图表
├── engine/
│   ├── frameCodec.ts         # protobufjs 解码 + 点云/图像/RawData 解码 + estimateFrameBytes
│   ├── frameDecoder.ts       # FrameDecoderPool 解码 Worker 池(round-robin)
│   ├── decodeWorker.ts       # 解码 Worker(module)：decodeFrame + 零拷贝 transfer 回传
│   ├── threeEngine.ts        # Three.js 渲染引擎 + 分段缓存/按需订阅/本地时钟播放
│   ├── FrameStream.ts        # WebSocket 协议层(8 类消息 + 控制/订阅指令)
│   ├── engine.ts             # three→stream→mock 级联
│   └── mockEngine.ts         # 无后端 mock 数据
├── tauri.ts                  # isTauri / pickMcapPath（零改动）
├── types.ts / index.css
```

**适配要点**：
- 引擎徽标文案「WASM 渲染」→「Three.js 渲染」；标题→「Three.js Viz」。
- 删除 App 层手动鼠标交互 useEffect，交给 `OrbitControls`。
- 图表数据从解码的 `frame.charts` 缓存，供图表面板消费（替代原 WASM `getFrameCharts`）。

---

## 7. Tauri 桌面外壳（`src-tauri/`）

- Tauri v2，`tauri-plugin-dialog` 提供原生文件对话框。
- `tauri.conf.json`：`productName="Three.js Viz"`、`identifier=com.threejs.viz`、
  `devUrl=localhost:5173`、`frontendDist=../dist`、窗口标题「Three.js Viz」1440x900。
- `capabilities/default.json`：`core:default` + `dialog:default` + `dialog:allow-open`。
- 桌面模式「浏览…」→ `pickMcapPath()`（原生对话框）→ `openByName(绝对路径)` 直开。

> **打包注意**：protobufjs 加载 `public/frame.proto`，Tauri 打包后需确保 asset
> 协议路径可 `fetch`。

---

## 8. 里程碑（重构进度）

| 阶段 | 目标 | 状态 |
| --- | --- | --- |
| **T1 工程骨架** | Vite + React + TS 工程、依赖、样式、可复用文件迁移 | ✅ |
| **T2 协议解码** | `public/frame.proto` + `frameCodec.ts`（protobufjs） | ✅ |
| **T3 渲染引擎** | `threeEngine.ts`（Three.js 渲染 + 图层几何映射 + 相机跟随） | ✅ |
| **T4 引擎接线** | `engine.ts` three→stream→mock 级联 + App 适配 | ✅ |
| **T5 组件迁移** | PlaybackBar / LayerPanel / ChartPanel | ✅ |
| **T6 Tauri 迁移** | `src-tauri/` 全套改名 Three.js Viz + icons | ✅ |
| **T7 构建验证** | `npm run build`（tsc + vite）通过，产物 gzip ~553 kB | ✅ |
| **T8 实机验收** | 连后端渲染真实 MCAP，F2–F7 功能对齐；Tauri 桌面实机 | ✅ 已接后端实机 |

### 8.1 实机后的迭代（任务一~四）

| 任务 | 内容 | 状态 |
| --- | --- | --- |
| **任务一** | 图层面板滚动/三态勾选、seek 即时渲染目标帧（不等下一轮 rAF） | ✅ |
| **任务二** | 解码 Worker 池（`FrameDecoderPool`）突破主线程 ~23 Hz 解码瓶颈 | ✅ |
| **任务三** | 分段缓存重构（`SEGMENT_SEC`/`ensureRange`/滑动窗口预取 + 字节背压） | ✅ |
| **任务四** | 图像/点云/RawData 按需订阅（勾选才拉取 + `refetchAroundCurrent` 回填） | ✅（点云占位透传） |
| — | 转弯时间补偿（`egoPoseAt` 按采样时刻插值 ego 位姿）修复整簇偏航 | ✅ |

---

## 9. 风险与对策

| 风险 | 影响 | 对策 |
| --- | --- | --- |
| 坐标系变换（z-up → y-up）视觉不一致 | ego 跟随/box 朝向/polygon 闭合偏差 | 实机与原 Filament 比对；world group 绕 X `-90°` 校准 |
| `frame.proto` 与后端结构漂移 | 解码错位/渲染异常 | proto 为唯一真源，改后端须同步 `public/frame.proto` |
| LayerRenderer 每帧 clear+rebuild | 大数据量 GPU/CPU 开销 | 后续改增量更新（复用 geometry/material） |
| WebSocket 大 Frame 流背压 | 前端积压、seek 迟滞 | 帧序号 + 丢弃过期帧；后端节流；seek 跳窗重推 |
| Tauri 打包后 `fetch` frame.proto 失败 | 解码器初始化失败 | 确认 asset 协议路径；必要时改为构建期内联 proto |
| vite chunk 体积警告（~1800 kB） | 首屏加载慢 | `manualChunks` 拆分 three/echarts vendor chunk |
| Sprite 文本清晰度 | 文本图层模糊 | 可选换 `troika-three-text` 提升质量（增体积） |

---

## 10. 开放项

1. **点云真实解析**：当前点云为占位透传（`encoding="raw_msg"`），待解析 packed float
   `xyz/rgb/intensity` 并按 `colorMode` 上色（当前 `renderPointClouds` 已就绪，缺后端解码）。
2. **RawData format 语义分派**：`raw_data` 已原样透传字节，待按 `format` 做解析/可视化分派。
3. **性能优化**：LayerRenderer 增量更新；vendor chunk 拆分；BufferGeometry 批量。
4. **元数据接口/S3 凭证/本地缓存目录**：后端配置项，实机确认端点。
5. **文本渲染质量**：Sprite vs `troika-three-text` 取舍。
