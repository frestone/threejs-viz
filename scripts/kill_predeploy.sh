#!/usr/bin/env bash
# 构建前清理: 释放 8080 端口并结束遗留的 viz_backend / threejs-viz-desktop 进程。
# 由 npm run predeploy:kill 调用; 放在 .sh 里而不是内联到 package.json,
# 是因为 Windows 上 npm 会用 cmd.exe 执行 lifecycle 脚本, 内联的 POSIX
# 引号(如 2>/dev/null、[ -n ... ])会被 cmd 误解析报错。

set -uo pipefail

# Windows(Git Bash)下 fuser/pkill 通常不可用, 用 taskkill 代替。
# macOS 无 fuser, 用 lsof 释放端口。
# 未安装对应命令也不阻塞构建: 找不到命令时退出码会被忽略。
if command -v taskkill >/dev/null 2>&1; then
  # 按端口 8080 找进程: netstat 输出形如 "TCP 0.0.0.0:8080 ... PID"。
  taskkill //F //PID \
    "$(netstat -ano | awk '$4 ~ /:8080$/ && $1 ~ /^TCP/ {print $5}' | sort -u)" \
    2>/dev/null || true
  taskkill //IM viz_backend.exe //F 2>/dev/null || true
  taskkill //IM threejs-viz-desktop.exe //F 2>/dev/null || true
elif [[ "$(uname -s)" == "Darwin" ]]; then
  lsof -ti tcp:8080 2>/dev/null | xargs kill -9 2>/dev/null || true
  pkill -x viz_backend 2>/dev/null || true
  pkill -x threejs-viz-desktop 2>/dev/null || true
else
  fuser -k 8080/tcp 2>/dev/null || true
  pkill -x viz_backend 2>/dev/null || true
  pkill -x threejs-viz-desktop 2>/dev/null || true
fi

sleep 0.5
exit 0
