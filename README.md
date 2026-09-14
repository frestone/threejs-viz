# Three.js Viz

自动驾驶数据可视化前端 —— React + Vite + TypeScript，渲染核心用 **Three.js**（WebGL），
桌面端用 **Tauri** 装载同一份构建。后端（厚后端 mcap 解码 + WebSocket Protobuf Frame
推流）保持不变。

> 架构说明见 [`docs/WEB_ARCHITECTURE.md`](docs/WEB_ARCHITECTURE.md)，需求见
> [`REQUIREMENTS.md`](REQUIREMENTS.md)，重构进度见 [`REFACTOR_TODO.md`](REFACTOR_TODO.md)。

## 环境要求

- Node.js ≥ 20.12（项目用 Vite 8/rolldown，需较新 Node）、npm
- 桌面构建额外需要 Rust 工具链 + 系统 WebView（Linux 需 WebKitGTK，Windows 用 WebView2）

## 安装依赖

```bash
npm install
```

> **Windows 注意**：项目用 Vite 8（rolldown），依赖里 `@vitejs/plugin-react@4` 与
> `vite@8` 有 peer 冲突，请用 `npm install --legacy-peer-deps`。
> 若 `node_modules/.bin` 下缺 `tsc.cmd`/`vite.cmd`（在 Git Bash 里 npm 6 装的依赖
> 不生成 `.cmd` shim，cmd 找不到命令），需用新 npm（≥10）重装依赖。

## Web 端

```bash
# 开发服务器（http://localhost:5173，热更新）
npm run dev

# 生产构建（tsc 类型检查 + vite 打包，产物在 dist/）
npm run build

# 本地预览生产构建
npm run preview
```

## 桌面端（Tauri）

```bash
# 桌面开发模式（自动起 Vite dev server + Tauri 窗口，一条命令构建+运行，带热更新）
npm run tauri:dev

# 桌面打包（先构建前端再编译 Rust，产物在 src-tauri/target/release/）
npm run tauri:build

# 一条命令：打生产包 + 自动运行生成的可执行文件
npm run tauri:build:run
```

## 清理编译中间产物

清理可重新生成的前端、Bazel 与 Tauri 编译产物及工具缓存（`dist/`、`build/`、
`bazel-*` 输出、`src-tauri/target/`、`nohup.out`、`test.log` 等）：

```bash
# 实际清理
npm run clean

# 仅预览将删除的路径（不执行删除）
npm run clean:dry
```

> **跨平台**：`npm run clean` 自动根据系统选择脚本 —— Windows 走
> `scripts/clear_cache.ps1`，Linux/macOS 走 `scripts/clear_cache.sh`。两套脚本
> 共享同一份产物清单、行为一致；也可直接运行对应脚本（`bash scripts/clear_cache.sh [--dry-run]`
> 或 `powershell -File scripts/clear_cache.ps1 [-DryRun]`）。
> Windows 下 PowerShell 对 `bazel-*` junction 使用非递归删除，不会误删链接指向的真实内容。

> **一条命令构建并运行**：开发场景用 `npm run tauri:dev`（`tauri.conf.json` 的
> `beforeDevCommand` 会自动起 Vite，再编译运行 Tauri 窗口）；生产场景用
> `npm run tauri:build:run`（打包后自动启动 `threejs-viz-desktop` 可执行文件）。

## 桌面端（Windows）

Windows 上用 MSVC 工具链 + vcpkg 构建整个 C++ FFI 后端（而非 Bazel 源码编译 FFmpeg）。

### Windows 环境准备

首次构建前需安装以下工具链：

- **MSYS2**（Bazel 在 Windows 上需要 bash），并设 `BAZEL_SH=C:\msys64\usr\bin\bash.exe`
- **Bazelisk / Bazel 7.4.1**（`bazel` 需在 PATH；项目根有 `.bazelversion` 钉版本）
- **vcpkg**（`D:\vcpkg`，设 `VCPKG_ROOT`），triplet 用 `x64-windows-static-md`：
  ```bash
  ./vcpkg install "ffmpeg[core,avcodec,swscale,swresample]" openssl zstd libjpeg-turbo \
    --triplet x64-windows-static-md --recurse
  ```
- **Rust 工具链**（`x86_64-pc-windows-msvc`）、**Node ≥ 20.12**、**Microsoft Visual Studio 2022（含 MSVC C++ 与 Windows SDK）**

### Windows 构建与运行

```bash
# 一键（推荐）：清残留进程 + 打 release 包 + 自动运行 threejs-viz-desktop.exe
npm run tauri:build:run:win

# 仅打 release 包（产物在 src-tauri/target/release/，或 CARGO_TARGET_DIR 指定位置）
npm run tauri:build:win

# 仅运行已构建的 release exe
bash scripts/run_release.sh
```

### Windows 安装包（NSIS）

```bash
# 打 NSIS 安装包（产物在 <target>/release/bundle/nsis/threeJs_VIZ_<ver>_x64-setup.exe）
npm run bundle:win
```

> **说明**：
> - `tauri.conf.json` 的 `bundle.targets` 设为 `["nsis"]`，`bundle.resources` 把
>   `server/configs/*.json` 打进安装包（安装后位于 exe 同目录的 `configs/`，C++
>   后端 `resolveConfigPath` 会按 exe 同目录回退读取，故安装版也能正常读配置）。
>   首次打包会从 GitHub 下载 NSIS 工具（自动，需联网）；如需同时产出裸 exe 与安装包，
>   可把 `bundle.targets` 改为 `["nsis", "none"]`。
> - 构建使用 `CARGO_TARGET_DIR`（如 `D:\threejs-viz-target`）时，exe、configs、安装包
>   均在该目录下（`release/`、`release/bundle/nsis/`）；`run_release.sh` 会自动识别
>   `CARGO_TARGET_DIR` 与 debug/release。
> - `build.rs` 在 Windows 上自动切到 vcpkg 静态库链接（`/WHOLEARCHIVE` 链接
>   `viz_ffi_archive.lib` + vcpkg 的 FFmpeg/OpenSSL/zstd/libjpeg），无需手动跑 Bazel。
> - **调试备选**：若需快速迭代，可用 `npm run tauri:build:run:win:debug`（debug 构建，
>   编译更快、含调试信息）。
> - **安全软件提示**：个别安全软件（360 安全卫士、corplink 等）会拦截 release 模式下
>   cargo 编译出的 build script 可执行文件（报 `拒绝访问 os error 5`）。若遇此问题，
>   退出/白名单安全软件后重跑 `npm run tauri:build:win` 即可。

## 一条命令构建并运行整个桌面应用

前后端一起：用 Bazel 构建 C++ 后端 → 后台启动后端（127.0.0.1:8080）→ 打生产包并运行 Tauri 桌面窗口。

```bash
# 构建后端 + 启动后端 + 打生产包并运行桌面应用（一条命令搞定）
npm run app
```

> `app` 脚本内部依次执行：`bazel build //server:viz_backend` →
> 后台在 `server/` 目录启动 `../bazel-bin/server/viz_backend 8080` → `npm run tauri:build:run`。
> 后端从工作目录读取 `configs/decoder.json` 与 `configs/s3.json`，故须在 `server/` 目录下运行。
> 前提：已 `npm install`，并安装了 Rust 工具链与 Bazel。
> 也可单独用 `npm run backend:build`（Bazel 构建后端）、`npm run backend:run`（运行后端）。

## 连接后端

前端通过 WebSocket 接收后端推送的 Protobuf `Frame`：

- 后端二进制帧格式：`[u8 msgType][u64 frameSeq] + Protobuf Frame bytes`
- 协议定义：[`public/frame.proto`](public/frame.proto)（protobufjs 运行时加载）
- 启动本地后端解码进程后，前端引擎按 `three → stream → mock` 级联接入

无后端时前端自动回退到 mock 数据，可直接 `npm run dev` 预览界面。

## 目录结构

```
src/
├── App.tsx / main.tsx        # React 入口与主界面
├── components/               # PlaybackBar / LayerPanel / ChartPanel
├── engine/
│   ├── frameCodec.ts         # protobufjs 解码 Frame
│   ├── threeEngine.ts        # Three.js 渲染引擎
│   ├── FrameStream.ts        # WebSocket 协议层
│   ├── engine.ts             # three→stream→mock 级联入口
│   └── mockEngine.ts         # 无后端 mock 数据
├── tauri.ts / types.ts / index.css
public/frame.proto            # Protobuf schema 副本
src-tauri/                    # Tauri 桌面外壳（Rust）
```