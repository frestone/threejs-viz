// 前端图像链路耗时记录（供分析实时播放卡顿）。
//
// 记录三段：大数据帧到达主线程(arrival) → rAF 中建 Blob URL(blob) → <img> onLoad 上屏(load)。
// 桌面端经 Tauri 命令 ffi_perf_log 追加到 $HOME/threejs-viz-perf/frontend_image.csv；
// 浏览器端无文件系统权限时仅回退 console。
//
// 批量发送：主线程每帧都做 IPC/文件写会反过来影响播放，故按 200 行或 500ms 合并一次。

type Mark = { arrived: number; blob?: number };

// Debug 模式开关：仅开启时记录与落盘；默认关闭（打包/生产零开销）。
// 桌面端由 App 启动时查询 Tauri ffi_is_debug（VIZ_DEBUG / --debug）；
// 浏览器端由 ?debug=1 或 dev 构建开启。
let debugEnabled = false;

const marks = new Map<string, Mark>();
let buffer: string[] = [];
let flushTimer: ReturnType<typeof setTimeout> | null = null;
let pathLogged = false;
let headerSent = false;

const key = (channel: string, seq: number) => `${channel}#${seq}`;
const fmt = (v: number | "") => (v === "" ? "" : v.toFixed(2));

export function setDebugEnabled(enabled: boolean): void {
  debugEnabled = enabled;
  if (!enabled) {
    // 关闭时清空悬空记录与待发缓冲，避免恢复后混入陈旧行。
    marks.clear();
    buffer = [];
    if (flushTimer !== null) {
      clearTimeout(flushTimer);
      flushTimer = null;
    }
  }
}

export function isDebugEnabled(): boolean {
  return debugEnabled;
}

async function flush(): Promise<void> {
  if (!debugEnabled) return;
  if (flushTimer !== null) {
    clearTimeout(flushTimer);
    flushTimer = null;
  }
  if (buffer.length === 0) return;
  const payload = buffer.join("\n");
  buffer = [];
  try {
    const { invoke } = await import("@tauri-apps/api/core");
    const path = await invoke<string>("ffi_perf_log", { batch: payload });
    if (path && !pathLogged) {
      pathLogged = true;
      console.log(`[perf] frontend image timing -> ${path}`);
    }
  } catch {
    if (!pathLogged) {
      pathLogged = true;
      console.log("[perf] frontend image timing (console fallback; Tauri unavailable)");
    }
    console.debug(payload);
  }
}

function push(line: string): void {
  if (!headerSent) {
    headerSent = true;
    buffer.push("wall_ms,channel,seq,arrival_to_blob_ms,blob_to_load_ms,total_ms");
  }
  buffer.push(line);
  if (buffer.length >= 200) {
    void flush();
    return;
  }
  if (flushTimer === null) {
    flushTimer = setTimeout(() => void flush(), 500);
  }
}

// 后端大数据帧到达主线程（threeEngine.onBigData 接受该帧时）。
export function noteBigDataArrival(channel: string, seq: number): void {
  if (!debugEnabled) return;
  if (!Number.isFinite(seq) || seq <= 0) return;
  marks.set(key(channel, seq), { arrived: performance.now() });
  // 极端情况下 onLoad 未触发（窗口关闭/图像被替换）会留下悬空记录，做有界清理。
  if (marks.size > 256) {
    const oldest = marks.keys().next().value;
    if (oldest !== undefined) marks.delete(oldest);
  }
}

// rAF 中为该帧创建 Blob URL（App.queueImageUpdate）。
export function noteBlobCreated(channel: string, seq: number): void {
  if (!debugEnabled) return;
  const mark = marks.get(key(channel, seq));
  if (mark) mark.blob = performance.now();
}

// <img> onLoad（CameraOverlay 图像真正完成解码上屏）。
export function noteImageLoaded(channel: string, seq: number | undefined): void {
  if (!debugEnabled) return;
  if (seq === undefined || !Number.isFinite(seq) || seq <= 0) return;
  const k = key(channel, seq);
  const mark = marks.get(k);
  if (!mark) return;
  marks.delete(k);
  const end = performance.now();
  const blobMs = mark.blob !== undefined ? mark.blob - mark.arrived : "";
  const loadMs = mark.blob !== undefined ? end - mark.blob : "";
  push(
    `${Date.now()},${channel},${seq},${fmt(blobMs)},${fmt(loadMs)},${fmt(end - mark.arrived)}`,
  );
}
