#!/usr/bin/env bash
# 运行构建好的 threejs-viz-desktop。
# Linux 无显示环境时自动用 xvfb-run(需已安装); Windows 直接启动即可。
# 注意: 该脚本会一直占用终端, 由 `exec` 替换当前 shell, 不要 Ctrl-C 后重跑——重复调用会反复 exec 到同一进程。

set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

# 优先用 CARGO_TARGET_DIR(若设置);否则默认 src-tauri/target。支持 debug/release。
# Windows 二进制带 .exe 后缀;Linux 无。
if [[ -n "${CARGO_TARGET_DIR:-}" ]]; then
  TARGET_ROOT="$CARGO_TARGET_DIR"
else
  TARGET_ROOT="$ROOT/src-tauri/target"
fi
BIN="$TARGET_ROOT/release/threejs-viz-desktop.exe"
[[ -f "$BIN" ]] || BIN="$TARGET_ROOT/release/threejs-viz-desktop"
[[ -f "$BIN" ]] || BIN="$TARGET_ROOT/debug/threejs-viz-desktop.exe"
[[ -f "$BIN" ]] || BIN="$TARGET_ROOT/debug/threejs-viz-desktop"

if [[ ! -f "$BIN" ]]; then
  echo "[run] 未找到 $BIN, 请先 npm run tauri:build(或 :debug)" >&2
  exit 1
fi

if [[ -n "${DISPLAY:-}" || -n "${WAYLAND_DISPLAY:-}" ]]; then
  exec "$BIN"
elif command -v xvfb-run >/dev/null 2>&1; then
  exec xvfb-run -a "$BIN"
else
  echo "[run] 无显示环境且未安装 xvfb-run, 直接尝试启动" >&2
  exec "$BIN"
fi
