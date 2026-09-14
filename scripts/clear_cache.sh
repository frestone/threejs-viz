#!/usr/bin/env bash
# 删除可重新生成的前端、Bazel 与 Tauri 编译产物及工具缓存。
# 默认执行清理；传入 --dry-run 仅打印将删除的路径。

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DRY_RUN=false

if [[ "${1:-}" == "--dry-run" ]]; then
  DRY_RUN=true
elif [[ $# -gt 0 ]]; then
  echo "用法: $0 [--dry-run]" >&2
  exit 2
fi

remove_path() {
  local path="$1"
  if [[ ! -e "$path" && ! -L "$path" ]]; then
    echo "[clean] skip: ${path#"$ROOT"/}"
    return
  fi

  if [[ "$DRY_RUN" == true ]]; then
    echo "[clean] would remove: ${path#"$ROOT"/}"
  else
    rm -rf -- "$path"
    echo "[clean] removed: ${path#"$ROOT"/}"
  fi
}

# 固定在项目根目录下操作，避免从其他目录调用时误删同名路径。
# 与 scripts/clear_cache.ps1 保持一致：两边共用同一份产物清单。
ARTIFACTS=(
  "$ROOT/dist"
  "$ROOT/node_modules/.vite"
  "$ROOT/src-tauri/target"
  "$ROOT/build"
  "$ROOT/bazel-bin"
  "$ROOT/bazel-out"
  "$ROOT/bazel-testlogs"
  "$ROOT/bazel-threejs-viz"
  "$ROOT/nohup.out"
  "$ROOT/test.log"
)

# Bazel 的 bazel-* 通常只是输出符号链接，需执行 clean 才会清除真实输出树。
if command -v bazel >/dev/null 2>&1; then
  if [[ "$DRY_RUN" == true ]]; then
    echo "[clean] would run: bazel clean"
  else
    echo "[clean] running: bazel clean"
    (cd "$ROOT" && bazel clean)
  fi
else
  echo "[clean] skip: bazel clean (bazel command not found)"
fi

for artifact in "${ARTIFACTS[@]}"; do
  remove_path "$artifact"
done

# TypeScript 增量编译信息可能位于根目录或子项目中。
while IFS= read -r -d '' tsbuildinfo; do
  remove_path "$tsbuildinfo"
done < <(find "$ROOT" -path "$ROOT/node_modules" -prune -o \
  -type f -name '*.tsbuildinfo' -print0)

if [[ "$DRY_RUN" == true ]]; then
  echo "[clean] dry run complete"
else
  echo "[clean] complete"
fi