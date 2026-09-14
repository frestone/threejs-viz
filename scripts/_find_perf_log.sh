#!/bin/bash
# 查找 viz_img_perf 日志可能的位置
set +e

echo "=== 1. 在常见日志文件中 grep ==="
for f in /tmp/viz_live2.log /tmp/viz8080.log /tmp/viz_scwd.log /tmp/tzv_v.log \
         /tmp/viz_live.log /tmp/viz.log /tmp/desktop.log \
         nohup.out bazel-threejs-viz/nohup.out \
         /home/thor/project/threejs-viz/nohup.out; do
    if [ -r "$f" ]; then
        n=$(grep -c "viz_img_perf" "$f" 2>/dev/null)
        echo "[$n hits] $f"
    fi
done

echo
echo "=== 2. 全 /tmp 扫描近期包含 viz_img_perf 的文件 ==="
grep -rl "viz_img_perf" /tmp 2>/dev/null | head -20

echo
echo "=== 3. 全家目录扫描 ==="
grep -rl "viz_img_perf" /home/thor 2>/dev/null | head -20

echo
echo "=== 4. 当前在跑的 threejs-viz-desktop 进程 ==="
pgrep -af threejs-viz-desktop 2>/dev/null

echo
echo "=== 5. 当前在跑的 viz_backend 进程(若有) ==="
pgrep -af viz_backend 2>/dev/null

echo
echo "=== 6. 进程 fd2 走向 ==="
for pid in $(pgrep -f threejs-viz-desktop 2>/dev/null) $(pgrep -f viz_backend 2>/dev/null); do
    echo "PID=$pid fd2=$(readlink /proc/$pid/fd/2 2>/dev/null)"
done

echo
echo "=== 7. binary mtime vs source mtime(确认构建已包含新代码) ==="
BIN=./src-tauri/target/release/threejs-viz-desktop
SRC=./server/viz-core/src/session/offline_session.cpp
[ -f "$BIN" ] && stat -c "%y  %n" "$BIN"
[ -f "$SRC" ] && stat -c "%y  %n" "$SRC"