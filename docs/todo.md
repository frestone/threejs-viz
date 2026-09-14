# 项目待办清单

更新时间：2026-09-08

> 本清单汇总当前仓库可确认的未完成事项。第三方目录中的 TODO/FIXME 不纳入项目待办。
> `REFACTOR_TODO.md` 中部分早期条目可能已被后续实现覆盖，执行前须按当前代码重新核验。

## P1-B：Tauri 进程内 FFI 双路径

### P1-7：接入桌面进程内 FFI

- [x] 在 Tauri 状态中持有线程安全的 `VizSession`，替代桌面 sidecar 子进程。
- [x] 暴露 open、close、seek、暂停、倍速和 startPrefetch 命令。
- [x] 暴露图像、点云和 RawData 的按需订阅命令。
- [x] 将 C++ 回调收到的完整字节封包通过 Tauri event 转发给前端。
- [x] 在前端实现 `FfiTransport`，并复用 WebSocket 路径的消息解析逻辑。
- [x] Web 模式继续使用 WebSocket transport，桌面模式自动选择 `FfiTransport`。
- [x] 从实际桌面运行路径调用 `ensure_linked`，防止 release 链接回收 C ABI 符号。
- [x] 移除桌面启动、退出阶段对 `viz_backend` sidecar 的管理代码。

### P1-8：健壮性与生命周期

- [ ] 验证 Rust 回调 panic 不会跨越 C ABI 边界。
- [ ] 验证 C++ 回调线程到 Tauri event 的线程安全性。
- [ ] 明确会话关闭、窗口退出和异常路径的资源释放顺序。
- [ ] 防止关闭后回调、重复销毁及悬空 callback context。
- [ ] 验证打开新数据源时旧会话、缓存与订阅状态正确重置。

### P1-9：双路径验收

- [ ] 运行并保留 FFI 字节契约测试结果。
- [ ] 验证 WS 与 FFI 的消息类型、帧序号及 payload 完全一致。
- [ ] 验证 Web/Vite 生产构建。
- [ ] 验证 Tauri debug 与 release 构建。
- [ ] 端到端验证打开、播放、暂停、seek、倍速和预取。
- [ ] 端到端验证图层、图表、图像、点云与 RawData。
- [ ] 检查进程退出后无线程、端口或文件句柄残留。

## 数据链路功能

- [ ] 实现点云 protobuf 反射解析：按 `listPath` 与 x/y/z/intensity tag 生成 packed float。
- [ ] 将真实点云消息纳入 MCAP 帧索引或按需读取链路，替换空点云占位。
- [ ] 评估并清理 `DemoDataSource::load` 的合成数据占位实现；真实链路统一走 MCAP 数据源。
- [ ] 用真实数据确认 WebSocket 协议仍为 type、seq 与 Frame protobuf payload。
- [ ] 确认 charts 定义继续由配置和消息下发，前端没有新增硬编码。
- [ ] 验证本地文件缓存优先与在线 S3 流式读取使用统一打开入口。

## 功能与实机验收

- [ ] Web 模式连接真实后端并持续渲染真实 MCAP 帧。
- [ ] 验证自车几何、位置、朝向及相机跟随。
- [ ] 验证规划轨迹、路径和 ribbon 渲染。
- [ ] 验证感知与预测目标的 box、polygon、类型名和颜色。
- [ ] 验证播放、暂停、seek、时间轴、倍速及缓存续供。
- [ ] 验证相机旋转、缩放和平移。
- [ ] 验证图层树、可见性切换与配置下发内容一致。
- [ ] 验证图表面板显隐、信号名称和数据正确。
- [ ] 对齐真实数据的 recognized 帧数与桌面基线。
- [ ] 实机验证“浏览”选择本地 MCAP 后的绝对路径打开链路。
- [ ] 实机验证 Tauri App 的渲染与交互行为和 Web 模式一致。

## 构建、发布与安全

- [ ] 正式分发前移除仓库配置中的开发凭据，改从用户配置目录或安全凭据源读取。
- [ ] 确认序列化、日志和安装包均不包含 S3 密钥等敏感信息。
- [ ] 验证 Tauri release/deb 包包含所需配置和静态链接依赖，不再依赖 sidecar。
- [ ] 在目标 Linux 环境验证 WebKitGTK、安装、启动、卸载及升级流程。
- [ ] 检查 release 二进制中的 FFI 与 FFmpeg 必需符号未被链接器回收。

## 清理与维护

- [ ] 删除已废弃的 `src-tauri/link_inputs.cquery`。
- [ ] 更新仍描述 sidecar/WebSocket 桌面架构的注释与文档。
- [ ] 复核 `REFACTOR_TODO.md` 第 2、7、8、9 节，将已完成但未勾选的条目归档。
- [ ] 确认旧 WASM、Filament、embind 与 Bazel wasm 配置是否仍有项目自有残留，再决定删除。
- [ ] 清理仅用于本地诊断的临时产物，但保留有价值的可复现测试脚本。

## 已完成的近期里程碑

- [x] P1-6：Bazel 聚合归档与 Rust 静态链接已打通。
- [x] P1-6：FFmpeg、zstd、OpenSSL 等链接依赖已补齐。
- [x] P1-6：Rust FFI 安全封装与回调 panic 隔离已实现。
- [x] P1-6：`cargo build` 成功，且二进制符号检查确认 FFI/FFmpeg 为真实强定义。
- [x] P1-7：桌面进程内 FFI transport 已接入，WebSocket 双路径保留。
- [x] P1-7：前端与 Rust debug 构建通过，Xvfb 下桌面持续运行且无 sidecar/8080 监听。