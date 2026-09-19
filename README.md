# Three.js Viz

自动驾驶数据可视化前端 —— React + Vite + TypeScript，渲染核心用 **Three.js**（WebGL），
桌面端用 **Tauri** 装载同一份构建。后端（厚后端 mcap 解码 + WebSocket Protobuf Frame
推流）保持不变。

> 架构说明见 [`docs/WEB_ARCHITECTURE.md`](docs/WEB_ARCHITECTURE.md)，需求见
> [`REQUIREMENTS.md`](REQUIREMENTS.md)，重构进度见 [`REFACTOR_TODO.md`](REFACTOR_TODO.md)。

## 环境要求

- Node.js ≥ 20.12（项目用 Vite 8/rolldown，需较新 Node）、npm
- 桌面构建额外需要 Rust 工具链 + 系统 WebView（macOS 用系统 WKWebView，装
  Xcode Command Line Tools 即可；Linux 需 WebKitGTK，Windows 用 WebView2）

> **macOS `npm: command not found`**：说明 Node 不在当前 zsh 的 PATH。若已用
> 官方包安装到 `~/.local/node-v22.14.0-darwin-arm64`，先在本终端加入 PATH：
>
> ```bash
> export PATH="$HOME/.local/node-v22.14.0-darwin-arm64/bin:$HOME/.local/bin:$HOME/.cargo/bin:$PATH"
> command -v npm && npm --version
> ```
>
> 或用 Homebrew 安装：`brew install node`。

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

## 导出 3DGS 数据集（图片 / 外参 / 主车位姿）

`scripts/export_gs_dataset.py` 从一条 ROVER MCAP 里导出 3D Gaussian Splatting 需要的三部分
数据：按观测时间命名的相机图片、相机内参+外参（来自 MCAP 标定附件
`sensor_calib_param.conf`）、以及 `/localization/odometry_location` 主车轨迹，并生成
nerfstudio 可直接读取的 `transforms.json`（逐帧 camera-to-world）。

```bash
python3 -m venv .venv-gs
.venv-gs/bin/pip install -r scripts/requirements-gs-export.txt

.venv-gs/bin/python scripts/export_gs_dataset.py \
  --mcap /path/to/record.mcap \
  --out  /path/to/gs_dataset \
  --cameras all
```

输出 `manifest.json` / `calibration.json` / `ego_pose.{csv,json}` /
`<camera>/transforms.json` 与 `<camera>/images/<观测时间>.jpg`，另有 `all/transforms.json`
供多相机合并训练。完整参数与输出说明见 [`docs/GS_EXPORT.md`](docs/GS_EXPORT.md)。

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

## 桌面端（macOS）

macOS 用系统 clang/libc++ + Apple ld 构建同一份 C++ FFI 后端（无需 vcpkg，也无需
手写 Bazel 链接参数），系统依赖统一走 Homebrew。

### macOS 环境准备

```bash
# 1) Xcode Command Line Tools：提供 clang/clang++/ar/ranlib/make 与系统 WKWebView 头
xcode-select --install

# 2) Homebrew（https://brew.sh），然后装系统依赖
brew install node zstd openssl@3 jpeg bazelisk

# 3) Rust 工具链（Tauri 外壳）
curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh
```

- **Bazel**：`bazelisk` 会按仓库根的 `.bazelversion` 拉取 Bazel 7.4.1。桌面 FFI
  聚合静态库用 `cc_static_library`（需 7.4+ 且 `--experimental_cc_static_library`，
  `src-tauri/build.rs` 已自动传该 flag）。
- **Homebrew 系统依赖**：`zstd`（MCAP chunk 解压）、`openssl@3`（SigV4/HTTPS，
  keg-only）、`jpeg`（缩略图重编码）。Bazel 侧
  （`server/third_party/sys_prefix.bzl`）与 `build.rs` 会自动探测前缀：先是
  `VIZ_SYS_PREFIX`，再是 Homebrew 默认前缀（Apple Silicon `/opt/homebrew`，Intel
  `/usr/local`），然后是 `brew --prefix`。用 conda/MacPorts/自定义目录装这几个库时
  显式指定即可：
  ```bash
  export VIZ_SYS_PREFIX=/your/prefix   # 需含 include/{zstd.h,openssl/,jpeglib.h} 与 lib/
  ```
- **必须是 OpenSSL 3.x**：asio 1.28（HTTPS/SigV4 底层）与 OpenSSL 4 不兼容
  （`ASN1_STRING` 变成不完整类型，编译期报
  `member access into incomplete type 'ASN1_STRING'`）。Homebrew 的 `openssl@3`
  即为 3.x；用 `VIZ_SYS_PREFIX` 指向其他前缀时注意别选到 4.x。
- **Cocoa 窗口**：Tauri 在 macOS 上直接用系统 WKWebView，不需要额外 WebView 依赖。

### macOS 构建与运行

```bash
# Web 端（无桌面外壳）
npm run dev

# 桌面开发模式：自动起 Vite dev server + Tauri 窗口，带热更新
npm run tauri:dev

# 桌面生产打包 + 自动运行（.app 在 src-tauri/target/release/bundle/macos/，
# .dmg 在 src-tauri/target/release/bundle/dmg/）
npm run tauri:build:run:mac
```

> 说明：
> - `npm install` 若报 `ERESOLVE`（`vite@8` 与 `@vitejs/plugin-react@4` 的 peer
>   冲突），改用 `npm install --legacy-peer-deps`。
> - `src-tauri/tauri.macos.conf.json` 把 `bundle.targets` 覆盖为 `["app", "dmg"]`
>   （基线配置里的 `nsis` 是 Windows 专用），Tauri 构建时自动按平台合并该文件。
> - macOS 链路与 Linux 的主要差异：Apple ld 用 `-force_load <archive>` 取代 GNU
>   的 `--whole-archive`；C++ 运行时是 libc++（`c++`）而非 libstdc++（`stdc++`）；
>   系统库按探测到的前缀给出 `-L/-I`（`openssl@3` 为 keg-only），并按需写
>   LC_RPATH。FFmpeg 三个静态库仍与 Linux 一样由 `@ffmpeg`（rules_foreign_cc
>   源码编译）产出，Rust 侧按 `bazel-out/darwin*-fastbuild/` 递归定位，不硬编码
>   CPU 目录名。
> - FFmpeg 的构建在 macOS 上改用系统 make（`server/third_party/system_make.bzl`
>   注册 macOS 专用 make toolchain）：rules_foreign_cc 自举的 GNU Make 4.4.1 跑
>   FFmpeg 的 Makefile 会段错误，而 Apple 自带 make 正常。
> - Homebrew 的 `zstd`/`openssl@3`/`jpeg` 按动态库链接，产物在本机（已装 Homebrew）
>   可直接运行；要分发到无 Homebrew 的机器，需改为静态链接或把 dylib 一并打包。
> - 只调前端时可跳过 FFI 链接：`VIZ_SKIP_FFI_LINK=1 npm run tauri:dev`（桌面 FFI
>   不可用，引擎回退到 mock 数据）。
> - **系统依赖不在 Homebrew 默认前缀时**，非默认前缀的 dylib 安装名可能是
>   `@rpath/...`，`build.rs` 已把该前缀的 `lib` 目录写进 LC_RPATH，产物可直接运行。
>
> **故障排查**：若构建时报
> `ld: tapi error: malformed file ... error: unknown architecture arm64e.x1-macos`，
> 说明本机 Command Line Tools 的默认 SDK 比自带的 `ld` 新（`xcrun --show-sdk-path`
> 指向的 SDK 无法链接任何 C 程序，`clang -o t t.c` 都会失败）。挑一个能链接的旧 SDK
> 固定给本次构建即可（`build.rs` 会把 `SDKROOT` 透传给 Bazel 的 repo 探测与 action 环境）：
>
> ```bash
> # 逐个候选 SDK 试链接，第一个成功的即可用
> export SDKROOT=/Library/Developer/CommandLineTools/SDKs/MacOSX26.5.sdk
> ```
>
> 也可以升级/重装 Command Line Tools 从根上解决。构建期用到的其它 macOS 适配都记录在
> 仓库里：`.bazelrc`（`-Wno-deprecated-builtins`、deployment target 11.0、统一
> `-std=c++17`，避免 absl/protobuf 与本仓库代码的 `string_view` ABI 不一致）、
> `server/third_party/system_make.bzl`（FFmpeg 用系统 make 构建）。

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

## 播放卡顿诊断（各阶段耗时文件）

性能诊断输出（`image_*.csv` 逐帧落盘、逐帧/每 50 帧日志、前端 perf 落盘）只在
**Debug 模式**下输出，打包/生产默认关闭——诊断本身会引入每帧字符串拼接、文件
I/O 与互斥锁，非 Debug 下热路径零开销，避免观测拖慢播放。

开启 Debug 模式（进程启动时，任选其一）：

```bash
# 桌面端：debug 构建（tauri dev / tauri build --debug）默认开启；
#         release 打包需显式开启：
export VIZ_DEBUG=1 ./src-tauri/target/release/threejs-viz-desktop
# 或
./src-tauri/target/release/threejs-viz-desktop --debug

# Web 后端（前后端分离部署）：
VIZ_DEBUG=1 ./bazel-bin/server/viz_backend 8080
# 或
./bazel-bin/server/viz_backend --debug 8080

# 浏览器端：URL 加 ?debug=1（dev 构建默认开启，vite build 打包默认关闭）
```

```bash
# 自定义输出目录（可选）；不设置则输出到 $HOME/threejs-viz-perf
export VIZ_PERF_LOG=/path/to/perf
```

开启后实时播放显示本地相机图像时，后端会为图像链路输出逐阶段耗时文件，便于
定位「读包 / HEVC 解码 / 排队 / 发送 / 前端上屏」各阶段占比：

| 文件 | 写入方 | 内容 |
| --- | --- | --- |
| `image_frames.csv` | C++ 后端 | 每批一行：本批下发帧数、读取微秒、解码微秒、其中 HEVC 解码/缩放/JPEG 编码微秒、喂帧数、是否 GOP 回退、JPEG 总字节数 |
| `image_stages.csv` | C++ 后端 | 每通道每 30 帧一行：平均读取/解码耗时、平均 HEVC/缩放/JPEG 耗时、平均喂帧数、GOP 回退次数、JPEG 平均大小 |
| `frontend_image.csv` | 前端(Tauri) | 每帧：大数据帧到达→建 Blob、Blob→`<img>` onLoad、端到端总耗时 |

正常连续播放时 `emit_frames` 与 `feed_frames` 应接近（区间内每帧都下发），`gop_reload=0`；
若长期出现大数值 `feed_frames` 与 `gop_reload=1`，说明窗口读取或播放头发生了断层。

图像解码落后播放头超过阈值时会丢弃中间帧、从最近 I 帧重同步以限制延迟，阈值可调：

```bash
export VIZ_IMAGE_MAX_CATCHUP_MS=500   # 默认 500ms；设 0 表示不跳帧、始终按序追补（延迟会累积）
```

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
