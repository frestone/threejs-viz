#!/bin/bash
cd /home/thor/project/threejs-viz
pkill -f viz_backend 2>/dev/null
sleep 1
bazel-bin/server/viz_backend 8080 > /tmp/viz8080.log 2>&1 &
SRV=$!
sleep 3
python3 _smoke_ws.py > /tmp/smoke_final.log 2>&1
echo "SMOKE_EXIT=$?" >> /tmp/smoke_final.log
kill "$SRV" 2>/dev/null
echo "=== server log ===" >> /tmp/smoke_final.log
cat /tmp/viz8080.log >> /tmp/smoke_final.log