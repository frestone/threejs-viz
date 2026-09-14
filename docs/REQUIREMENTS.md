# 自动驾驶可视化系统 — 需求与架构设计（Three.js 版）

> 目标：构建一套**桌面与 Web 统一架构**的自动驾驶数据可视化系统，
> 以 [Three.js](https://threejs.org/)（WebGL）为前端渲染核心，
> 能**快速适配不同数据源**（统一 MCAP 容器，厂家消息格式各异，用户仅需提供 protobuf 定义），
> 并支持**用户定制化的可视化需求**（通过声明式配置扩展图层，无需改渲染代码）。
>
> **重构说明**：本版本由原 Filament 版重构而来 —— 前端渲染核心从
> 「Emscripten WASM + Filament」替换为纯 JavaScript 的 **Three.js**；后端
> （厚后端 mcap 解码 + WebSocket Protobuf Frame 推流）链路保持不变。

---

## 1. 背景与动机

已有一个基于 **Bevy(Rust) + React** 的 MVP（`~/project/bevy-mvp`），验证了核心思路：

- 桌面原生与 Web(WASM) **共用同一套渲染核心**（`data.rs` + `viz.rs`）。
- 读取 MCAP，按 `log_time` 聚合成帧，回放自车/规划/感知。
- React 仅作 Web 外壳，3D 渲染交给引擎 canvas。
- 已有声明式的 `PlanningRenderConfig`（图层 → 字段路径 → 绘制模式 → 样式）雏形。

**痛点**：Rust + Bevy 生态相对不成熟（渲染特性、跨平台窗口、WASM 体积与兼容性、
第三方 proto/mcap 生态、长期维护成本）。曾一度改用工业级 C++ 引擎 **Filament** +
Emscripten WASM，但 WASM+Filament 工具链沉重、构建复杂、产物体积大（约 3.1MB）。
最终改用生态成熟、集成轻量的纯 JavaScript 引擎 **Three.js** 作为前端渲染核心。

## 2. 为什么选 Three.js

| 维度 | 说明 |
| --- | --- |
| 成熟度 | 社区最主流的 WebGL 3D 库，文档/示例/生态极其丰富 |
| 跨平台 | 浏览器原生 WebGL2；桌面经 **Tauri** 装载同一份 React 构建 |
| 集成轻量 | 纯 JS/TS 依赖（npm），无 Emscripten/WASM 工具链，构建简单、迭代快 |
| 产物体积 | 无 3.1MB WASM 包；JS 产物经 gzip 约 553 kB，显著更小 |
| 渲染能力 | Scene/Camera/Renderer/几何/材质/OrbitControls 满足本项目几何可视化需求 |
| 前端解码 | 配合 protobufjs 运行时解析 Frame，前端直接建几何渲染，无需 embind 绑定层 |

## 3. 总体架构

```
                    ┌──────────────────────────────────────────┐
                    │              可视化核心 (C++)               │
                    │  viz-core  (桌面 & Web 100% 复用)            │
                    │                                            │
   MCAP 文件 ─▶ ┌───────────┐   Frame 序列   ┌──────────────────┐ │
   protobuf ──▶ │ 数据抽象层  │ ──WS Protobuf─▶ │  Three.js 渲染层  │ │
   defs         │ DataSource │                │  Scene/View/Cam   │ │
                └───────────┘                └──────────────────┘ │
                    │  ▲                             ▲             │
                    ││ SourceProfile (JSON)        │ LayerConfig │
                    │  └── 消息格式映射                │  (JSON)     │
                    └──────────────────────────────────────────┘
                          │                              │
              ┌───────────┴──────────┐      ┌────────────┴───────────┐
              │  桌面壳 (Tauri)       │      │  Web 壳 (React + JS)    │
              │  同一 React 构建       │      │  Three.js WebGL         │
              └──────────────────────┘      └────────────────────────┘
```

三层解耦：

1. **厚后端数据层 `DataSource`（C++）**：读 MCAP → 按 schema 解码 → 归一化为统一 `Frame` → Protobuf 序列化 → WebSocket 推送。
2. **前端渲染核心 `Renderer`（TypeScript + Three.js）**：protobufjs 解码 `Frame`，按 `LayerConfig` 用 Three.js 绘制几何。
3. **平台壳**：Web（浏览器静态托管）与桌面（Tauri 装载同一份 React 构建），仅负责窗口/UI/事件，前端渲染核心零分叉。

## 4. 数据源快速适配（核心诉求一）

### 4.1 约束
- 容器统一为 **MCAP**。
- 不同厂家的消息 **payload 为 protobuf**，字段结构各异。
- 用户只需提供：**`.proto` 定义** + **一份 `SourceProfile` 映射配置**。

### 4.2 SourceProfile（声明式数据映射）
将"厂家消息 → 系统语义实体"的映射外置为 JSON，避免为每家改 C++：

```jsonc
{
  "sourceName": "vendorA",
  "protoDescriptorSet": "vendorA.desc",   // protoc --descriptor_set_out 产物
  "entities": {
    "ego": {                              // 自车定位
      "topic": "/localization/global_location",
      "messageType": "vendorA.Localization",
      "fields": {
        "x":   "odometry.pose.position.x",
        "y":   "odometry.pose.position.y",
        "z":   "odometry.pose.position.z",
        "yaw": "odometry.pose.euler.yaw"
      }
    },
    "trajectory": {                       // 规划轨迹（重复字段）
      "topic": "/planning/planning_result",
      "messageType": "vendorA.PlanningResult",
      "repeated": "trajectory.trajectory_point",
      "fields": { "x": "path_point.x", "y": "path_point.y" }
    },
    "obstacles": {                        // 感知目标列表
      "topic": "/perception/fused_track",
      "messageType": "vendorA.TrackList",
      "repeated": "tracks",
      "fields": {
        "id": "id", "type": "type",
       "x": "position.x", "y": "position.y", "z": "position.z",
        "heading": "heading",
        "length": "size.length", "width": "size.width", "height": "size.height"
      }
    }
  }
}
```

### 4.3 实现方式
- 使用 **protobuf 反射（DynamicMessage + FileDescriptorSet）**：运行时加载用户 `.desc`，
  无需为每家生成/编译代码即可按 `messageType` 解码任意消息。
- 字段路径（`a.b.c`）用反射逐级取值，`repeated` 展开为点集/目标列表。
- 归一化输出统一的 `Frame { ego, layers[], obstacles[] }`。

> MVP 阶段先内置一个演示 profile（JSON payload / 简化 proto），
> 反射式 DynamicMessage 作为下一步接入，接口预留。

## 5. 定制化可视化（核心诉求二）

沿用并强化 MVP 的声明式 `LayerConfig`：用户用 JSON 描述"画什么、怎么画"。

```jsonc
{
  "layers": [
    { "id": "trajectory", "source": "trajectory", "draw": "ribbon",
      "style": { "color": "#87CEFA", "opacity": 0.5, "width": 1.0, "height": 0.5 } },
    { "id": "path",       "source": "path",       "draw": "line",
      "style": { "color": "#FFD166", "width": 0.08 } },
    { "id": "obstacles",  "source": "obstacles",  "draw": "box",
      "style": { "colorByType": true } }
  ]
}
```

绘制模式（`draw`）可扩展：`line` / `ribbon` / `points` / `box` / `polygon` / `mesh` …
每种模式对应一个 `LayerRenderer`，注册进渲染核心，实现**开闭原则**。

## 6. 功能需求（MVP 范围）

| # | 功能 | MVP | 说明 |
| --- | --- | --- | --- |
| F1 | 加载 MCAP，聚合为帧序列 | ✅ | 先支持内置 demo 格式 |
| F2 | 自车渲染（box + 朝向） | ✅ | |
| F3 | 规划轨迹渲染（line/ribbon） | ✅ | 配置驱动 |
| F4 | 感知目标渲染（box/polygon） | ✅ | 配置驱动 |
| F5 | 播放/暂停/seek/时间轴 | ✅ | |
| F6 | 相机轨道控制（旋转/缩放/平移） | ✅ | |
| F7 | 图层可见性切换 | ✅ | |
| F8 | SourceProfile 动态映射 | 后端实现 | 反射式解码，后端 viz-core 落地 |
| F9 | Web + 桌面统一壳 | ✅ | React + Three.js（Web），Tauri 装载同一构建（桌面） |

## 7. 非功能需求
- **统一壳**：Web 与桌面共用同一份 React 构建，禁止 UI 逻辑分叉。
- **可扩展**：新增数据源 = 加 profile（后端）；新增可视化 = 加 Three.js 图层渲染器。
- **性能**：万级点/目标 60fps（Three.js 批量几何 / BufferGeometry）。
- **可移植**：前端 TypeScript + Three.js（Vite 构建）；后端 C++17（viz-core + protobuf + mcap）。

## 8. 目录结构

```
threejs-viz/                   # 前端（Web + Tauri 桌面）
├── REQUIREMENTS.md            # 本文档
├── docs/WEB_ARCHITECTURE.md   # Web 架构设计
├── package.json               # 前端依赖与脚本（Vite）
├── vite.config.ts             # 构建配置
├── index.html
├── public/
│   └── frame.proto            # Protobuf schema（protobufjs 运行时加载）
├── src/
│   ├── main.tsx / App.tsx     # React 入口与主界面
│   ├── components/            # PlaybackBar / LayerPanel / ChartPanel
│   ├── engine/                # 渲染引擎层
│   │   ├── frameCodec.ts      # protobufjs 解码 Frame
│   │   ├── threeEngine.ts     # Three.js 渲染引擎 + 图层几何映射
│   │   ├── FrameStream.ts     # WebSocket 协议层
│   │   ├── engine.ts          # three→stream→mock 级联入口
│   │   └── mockEngine.ts      # 无后端时的 mock 数据
│   ├── tauri.ts / types.ts
│   └── index.css
└── src-tauri/                 # Tauri 桌面外壳（Rust）
    ├──tauri.conf.json / Cargo.toml / build.rs
    ├── src/{main,lib}.rs / capabilities / icons

后端 viz-core（C++，独立仓，保持不变）：读 MCAP → 解码组装 Frame → Protobuf → WebSocket 推流。
```

## 9. 里程碑

- **M1（Filament 版）**：C++ viz-core 骨架 + 数据/渲染/配置抽象 + 桌面可编译运行（已完成，见 git 历史）。
- **M2**：接入 protobuf 反射，跑通真实厂家 MCAP + SourceProfile（后端）。
- **M3（Three.js 重构·本次）**：前端渲染核心由 WASM+Filament 改为 Three.js；protobufjs 解码 Frame + Three.js 建几何；Tauri 桌面外壳迁移；`npm run build` 通过。
- **M4**：实机连后端渲染真实 MCAP，F2–F7 功能验收；性能优化（BufferGeometry 批量 / LayerRenderer 增量更新 / vendor chunk 拆分）。