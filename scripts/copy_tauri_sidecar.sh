#!/usr/bin/env bash
# 把 Bazel 构建的 viz_backend 与 server/configs/ 拷贝到 Tauri release 目录,
# 让 `threejs-viz-desktop` 启动时 [`src-tauri/src/lib.rs:32`](src-tauri/src/lib.rs:32)
# `resolve_backend()` 第一段(同目录 sidecar)命中, 并让 viz_backend 能读到
# 相对路径 configs/*.json(`server/platform/web/server.cpp:225`)。
#
# idempotent: 重复运行只会覆盖既有文件, 无副作用。
# 不向 Tauri bundle 资源里塞, 也不动 .deb/.rpm targets: Tauri release 目录
# 与 Debian/RPM 包内容是分开的两件事, 这里只保证本机直接跑 ./threejs-viz-desktop 可行。

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
REL="$ROOT/src-tauri/target/release"

BIN_SRC="$ROOT/bazel-bin/server/viz_backend"
CFG_SRC="$ROOT/server/configs"

if [[ ! -x "$BIN_SRC" ]]; then
  echo "[sidecar] $BIN_SRC 不存在或不可执行, 请先跑 npm run backend:build" >&2
  exit 1
fi

mkdir -p "$REL"
cp -f "$BIN_SRC" "$REL/viz_backend"
chmod +x "$REL/viz_backend"

# rsync 风格: configs/* 平铺进 release/configs/, 后端相对路径读 configs/decoder.json 等。
mkdir -p "$REL/configs"
cp -f "$CFG_SRC"/*.json "$REL/configs/"

echo "[sidecar] copied: $BIN_SRC -> $REL/viz_backend"
echo "[sidecar] copied: $CFG_SRC/*.json -> $REL/configs/"
ls -1 "$REL/viz_backend" "$REL/configs" | sed 's/^/          /'