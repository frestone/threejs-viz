#!/bin/bash
# 从 server/ 目录启动服务（configs 相对路径可命中），验证 ProfileRegistry 选 decoder。
cd /home/thor/project/threejs-viz
pkill -f viz_backend 2>/dev/null
sleep 1
( cd server && exec ../bazel-bin/server/viz_backend 8080 ) > /tmp/viz_scwd.log 2>&1 &
SRV=$!
sleep 3
python3 _smoke_ws.py > /tmp/smoke_scwd.log 2>&1
echo "SMOKE_EXIT=$?" >> /tmp/smoke_scwd.log
kill "$SRV" 2>/dev/null
echo "=== server log ===" >> /tmp/smoke_scwd.log
cat /tmp/viz_scwd.log >> /tmp/smoke_scwd.log