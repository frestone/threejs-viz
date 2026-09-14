#!/bin/bash
OUT=/home/thor/project/threejs-viz/server/_calib_out.txt
: > "$OUT"
echo "=== ParseCalibrateConf files ===" >> "$OUT"
grep -rln "ParseCalibrateConf" /home/thor/code/xmonitor 2>/dev/null >> "$OUT"
echo "=== CalibrationParam .proto files ===" >> "$OUT"
grep -rln "CalibrationParam" /home/thor/code --include=*.proto 2>/dev/null | head -20 >> "$OUT"
echo "=== message CalibrationParam def ===" >> "$OUT"
grep -rn "message CalibrationParam" /home/thor/code --include=*.proto 2>/dev/null | head >> "$OUT"
echo "=== DONE ===" >> "$OUT"