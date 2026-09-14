#!/bin/bash
pkill -f viz_backend
sleep 1
cd /home/thor/project/threejs-viz/server
nohup /home/thor/project/threejs-viz/bazel-bin/server/viz_backend > /tmp/tzv_v.log 2>&1 &
BPID=$!
sleep 3
{
  echo "launched pid=$BPID"
  echo "alive:"
  pgrep -f viz_backend
  echo "logsize:"
  ls -la /tmp/tzv_v.log
  echo "===head==="
  head -30 /tmp/tzv_v.log
} > /tmp/tzv_launch_result.txt 2>&1