#!/bin/bash
# 启动常驻后端(脱离终端会话),供前端/冒烟连接。日志 /tmp/viz_live2.log。
cd /home/thor/project/threejs-viz/server
pkill -f viz_backend 2>/dev/null
sleep 1
setsid nohup ../bazel-bin/server/viz_backend 8080 > /tmp/viz_live2.log 2>&1 < /dev/null &
echo "PID=$!"