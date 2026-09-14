import type { CameraMode, EngineApi } from "../types";

// Mock 引擎：本地自驱动播放循环，无需 Three.js/WebSocket。
// 用 performance.now() 推进播放时间，供 UI 骨架联调进度条与图层开关。
// 真实实现见 threeEngine.ts（three 渲染 + FrameStream 推流）。
export interface MockEngineOptions {
  durationSec?: number;
  frameCount?: number;
}

export function createMockEngine(opts: MockEngineOptions = {}): EngineApi {
  const duration = opts.durationSec ?? 60;
  const frameCount = opts.frameCount ?? 3569; // 与真实 ROVER 基线一致，纯展示
  const visible = new Set<string>();

  let time = 0;
  let paused = true;
  let speed = 1;
  let lastTick = performance.now();
  let disposed = false;

  let cameraMode: CameraMode = "follow";
  // 内部时钟：只要未暂停就按 speed 推进 time，到末尾停住。
  const advance = () => {
    if (disposed) return;
    const now = performance.now();
    const dt = (now - lastTick) / 1000;
    lastTick = now;
    if (!paused) {
      time += dt * speed;
      if (time >= duration) {
        time = duration;
        paused = true;
      }
    }
    requestAnimationFrame(advance);
  };
  requestAnimationFrame(advance);

  return {
    setPaused(p: boolean) {
      // 从暂停恢复时重置 lastTick，避免 dt 累积跳变。
      if (paused && !p) lastTick = performance.now();
      paused = p;
      if (time >= duration && !p) time = 0; // 播完再点播放则回到开头
    },
    setSpeed(s: number) {
      speed = s;
    },
    seek(t: number) {
      time = Math.max(0, Math.min(duration, t));
    },
    setLayerVisible(layerId: string, v: boolean) {
      if (v) visible.add(layerId);
      else visible.delete(layerId);
    },
    getTime() {
      return time;
    },
    getDuration() {
      return duration;
    },
    getFrameCount() {
      return frameCount;
    },
    isPaused() {
      return paused;
    },
    getSpeed() {
      return speed;
    },
    setCameraMode(mode: CameraMode) {
      cameraMode = mode;
    },
    getCameraMode() {
      return cameraMode;
    },
    getDataMode() {
      return null;
    },
    getStats() {
      return { fps: 0, renderFps: 0, cacheFrames: 0, cacheBytesMB: 0, bufferAheadSec: 0, starving: false, frameBuildMs: 0, gpuRenderMs: 0, bufferedRatio: 0 };
    },
    dispose() {
      disposed = true;
    },
  };
}