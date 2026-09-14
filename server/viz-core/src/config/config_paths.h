#pragma once
// -----------------------------------------------------------------------------
// 配置路径解析（cwd 鲁棒性加固）。
//
// 背景：viz-core 各处用相对路径读配置（"configs/decoder.json" 等），历史上
// 依赖进程 cwd 恰好是 server/。一旦运行形态切换（bazel 从项目根启动、桌面
// Tauri 进程内 FFI、deb 安装后任意 cwd 启动），相对路径解析失败，静默退化
// 内置默认值——曾导致 light_map decoder 未加载、prediction 图层漂移的 bug。
//
// resolveConfigPath 按序尝试候选位置，返回首个真实存在的路径：
//   1) 原样（cwd 相对）——保持现有 server/ 开发行为，兼容 VIZ_DECODER_CONFIG
//   2) <exe_dir>/<path> —— 桌面二进制同目录（build.rs 会把 server/configs 复制
//      到 target/<profile>/configs）
//   3) /usr/bin/<path> —— deb 安装形态（tauri.conf.json bundle 把
//      resources/configs 装到 /usr/bin/configs）
// 绝对路径或候选均不存在时原样返回（保留调用方原有的"不可读"报错路径）。
// -----------------------------------------------------------------------------
#include <string>

namespace viz::config {

// 返回解析后的配置路径（见文件头注释的候选顺序）。
std::string resolveConfigPath(const std::string& path);

}  // namespace viz::config