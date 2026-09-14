import { useCallback, useEffect, useRef } from "react";

const SPEEDS = [0.25, 0.5, 1, 2];

function fmt(sec: number): string {
  if (!isFinite(sec) || sec < 0) sec = 0;
  const m = Math.floor(sec / 60);
  const s = Math.floor(sec % 60);
  const ms = Math.floor((sec - Math.floor(sec)) * 10);
  return `${String(m).padStart(2, "0")}:${String(s).padStart(2, "0")}.${ms}`;
}

export interface PlaybackBarProps {
  /** 兜底/初值：暂停、seek、加载后由上层回写；平滑推进不依赖它。 */
  time: number;
  duration: number;
  paused: boolean;
  speed: number;
  /**
   * 实时时钟读取器（引擎 getTime，基于 performance.now 现算）。
   * 进度条内部据此驱动，绕开被主线程渲染阻塞的 React 状态轮询。
   */
  getLiveTime?: () => number;
  /**
   * 已加载(缓存)到的最大帧时间戳读取器。进度条 CSS 平滑动画只推进到该边界，
   * 避免后端回填未跟上(starving)时进度条越过实际能渲染到的场景画面。
   * 未提供时退化为按 duration 匀速到底（旧行为）。
   */
  getLoadedTime?: () => number;
  /**
   * 可拖动上界读取器（引擎 getSeekableMax，返回已解码进内存的最大帧时间戳 loadedMaxTime）。
   * 方案A纯内存：拖动上界随内存缓存前沿放开；拖到未缓存区由引擎 seek 向后端请求续供帧。
   * 未提供时退化为不限制（可拖到 duration，旧行为）。
   */
  getSeekableMax?: () => number;
  onTogglePause: () => void;
  onSeek: (timeSec: number) => void;
  /**
   * 拖动进度条过程中的实时预览回调（可选）。与 onSeek(释放时提交) 区别：
   * scrub 在拖动途中高频触发，用于让场景画面实时跟随进度条位置。
   * 上层应绑定到引擎的轻量 seek（优先命中内存缓存帧、纯本地渲染）。
   */
  onScrub?: (timeSec: number) => void;
  /** 拖动开始（按下滑块）回调：上层据此开启引擎拖动预览锁。 */
  onScrubStart?: () => void;
  /** 拖动结束（释放/取消）回调：上层据此关闭拖动锁，随后 onSeek 提交正式跳转。 */
  onScrubEnd?: () => void;
  onSpeed: (speed: number) => void;
}

export function PlaybackBar(props: PlaybackBarProps) {
  const { time, duration, paused, speed } = props;
  // seeking 期间不让轮询回写覆盖用户拖动值。
  const seekingRef = useRef(false);
  const pointerSeekingRef = useRef(false);
  const pendingSeekRef = useRef<number | null>(null);
  const onScrubRef = useRef(props.onScrub);
  onScrubRef.current = props.onScrub;

  // 直接操作 DOM 的引用。
  const seekRef = useRef<HTMLInputElement>(null);
  const curLabelRef = useRef<HTMLSpanElement>(null);
  const pctLabelRef = useRef<HTMLSpanElement>(null);
  const fillRef = useRef<HTMLDivElement>(null); // 进度填充条（CSS 合成线程动画载体）
  const bufferTrackRef = useRef<HTMLDivElement>(null);
  const bufferFillRef = useRef<HTMLDivElement>(null);
  const cacheLabelRef = useRef<HTMLSpanElement>(null);

  // 用 ref 承接最新 props，供 rAF 闭包读取。
  const getLiveTime = props.getLiveTime;
  const getLoadedTime = props.getLoadedTime;
  const getSeekableMax = props.getSeekableMax;
  const getSeekableMaxRef = useRef(getSeekableMax);
  getSeekableMaxRef.current = getSeekableMax;
  // 把拖动/seek 目标 clamp 到已落盘上界：未落盘区域禁止拖入。
  // 留 1e-3 余量避免恰好落在边界帧时因浮点误差判为未缓存。
  const clampSeek = useCallback((value: number): number => {
    const max = getSeekableMaxRef.current?.();
    if (max === undefined || !Number.isFinite(max) || max <= 0) return value;
    return Math.min(value, max);
  }, []);
  const propTimeRef = useRef(time);
  const durationRef = useRef(duration);
  const pausedRef = useRef(paused);
  const speedRef = useRef(speed);
  propTimeRef.current = time;
  durationRef.current = duration;
  pausedRef.current = paused;
  speedRef.current = speed;

  // 播放进度严格由引擎最后实际渲染帧驱动；不再使用独立 CSS 匀速动画，
  // 避免主线程渲染落后时进度条先于画面。
  const syncFill = useCallback(() => {
    const el = fillRef.current;
    if (!el) return;
    const dur = durationRef.current;
    const cur = getLiveTime ? getLiveTime() : propTimeRef.current;
    const ratio = dur > 0 ? Math.min(1, Math.max(0, cur / dur)) : 0;
    el.style.transition = "none";
    el.style.transform = `scaleX(${ratio})`;
  }, [getLiveTime]);

  const lastScrubTargetRef = useRef<number | null>(null);
  // 原生 input 与 pointermove 都只上报目标位置；引擎侧 latest-only 消费器负责流控，
  // 每个渲染周期至多构建一帧。这里仅按时间去重，保留 pointermove 作为 WebView 兼容路径。
  const scrubNow = useCallback((value: number) => {
    if (lastScrubTargetRef.current !== null && Math.abs(lastScrubTargetRef.current - value) < 0.001) return;
    lastScrubTargetRef.current = value;
    onScrubRef.current?.(value);
  }, []);

  // 播放态 / 倍速 / 总时长变化时重设动画（不依赖 time，避免每 rAF 抖动重启）。
  useEffect(() => {
    syncFill();
  }, [paused, speed, duration, syncFill]);

  // 独立高频 rAF：更新滑块 value 与文字（读实时时钟），并检测 seek 跳变后重设 CSS 动画。
  useEffect(() => {
    let raf = 0;
    let lastRatio = -1;
    let lastLoaded = -1;
    const tick = () => {
      raf = requestAnimationFrame(tick);
      if (seekingRef.current) return; // 用户拖动中，别覆盖
      const t = getLiveTime ? getLiveTime() : propTimeRef.current;
      const dur = durationRef.current;
      if (seekRef.current && document.activeElement !== seekRef.current) {
        seekRef.current.value = String(t);
      }
      if (curLabelRef.current) curLabelRef.current.textContent = fmt(t);
      const ratio = dur > 0 ? t / dur : 0;
      if (pctLabelRef.current) {
        pctLabelRef.current.textContent = `${Math.round(ratio * 100)}%`;
      }
      // 检测非匀速跳变（seek / 末尾回绕 / starving 停顿）：偏差大则重设 CSS 动画。
      let needSync = lastRatio >= 0 && Math.abs(ratio - lastRatio) > 0.02;
      // 已加载边界随后端回填增长时，重设动画把进度条向前延伸到新的已加载点，
      // 使"进度条推进"与"实际能渲染到的场景边界"保持一致(节流到 >0.5% 才重设，避免频繁重启)。
      if (getLoadedTime) {
        const loaded = getLoadedTime();
        const loadedRatio = dur > 0 ? Math.min(1, Math.max(0, loaded / dur)) : 0;
        if (bufferFillRef.current) {
          bufferFillRef.current.style.transform = `scaleX(${loadedRatio})`;
        }
        bufferTrackRef.current?.setAttribute(
"title",
          `已缓存 ${fmt(loaded)} / ${fmt(dur)}（${Math.round(loadedRatio * 100)}%）`,
        );
        if (cacheLabelRef.current) {
          cacheLabelRef.current.textContent = `缓存 ${Math.round(loadedRatio * 100)}%`;
        }
        if (lastLoaded >= 0 && dur > 0 && Math.abs(loaded - lastLoaded) / dur > 0.005) {
          needSync = true;
        }
        lastLoaded = loaded;
      }
      if (needSync) syncFill();
      lastRatio = ratio;
    };
    raf = requestAnimationFrame(tick);
    return () => cancelAnimationFrame(raf);
  }, [getLiveTime, getLoadedTime, syncFill]);

  return (
    <div className="playback-bar">
      <button
        className="play-btn"
        onClick={props.onTogglePause}
        title={paused ? "播放" : "暂停"}
      >
        {paused ? "▶" : "⏸"}
      </button>

      <span className="time-label" ref={curLabelRef}>{fmt(time)}</span>

      <div className="seek-wrap">
        <input
          ref={seekRef}
          className="seek"
          type="range"
          min={0}
          max={duration}
          step={0.01}
          defaultValue={time}
          onPointerDown={(e) => {
            pointerSeekingRef.current = true;
            seekingRef.current = true;
            pendingSeekRef.current = null;
            lastScrubTargetRef.current = null;
            e.currentTarget.setPointerCapture?.(e.pointerId);
            props.onScrubStart?.();
          }}
          onPointerMove={(e) => {
            if (!pointerSeekingRef.current) return;
            const rect = e.currentTarget.getBoundingClientRect();
            if (rect.width <= 0) return;
            const ratio = Math.min(1, Math.max(0, (e.clientX - rect.left) / rect.width));
            const value = clampSeek(ratio * durationRef.current);
            pendingSeekRef.current = value;
            e.currentTarget.value = String(value);
            if (curLabelRef.current) curLabelRef.current.textContent = fmt(value);
            const dur = durationRef.current;
            if (pctLabelRef.current) {
              pctLabelRef.current.textContent = `${Math.round((dur > 0 ? value / dur : 0) * 100)}%`;
            }
            if (fillRef.current) {
              fillRef.current.style.transform = `scaleX(${dur > 0 ? Math.min(1, Math.max(0, value / dur)) : 0})`;
            }
            scrubNow(value);
          }}
          onPointerUp={(e) => {
            const value = clampSeek(pendingSeekRef.current ?? parseFloat(e.currentTarget.value));
            pointerSeekingRef.current = false;
            seekingRef.current = false;
            pendingSeekRef.current = null;
            // 先退出拖动锁再提交正式 seek（否则会再次走进引擎的拖动预览分支）。
            props.onScrubEnd?.();
            props.onSeek(value);
          }}
          onPointerCancel={() => {
            pointerSeekingRef.current = false;
            seekingRef.current = false;
            pendingSeekRef.current = null;
            props.onScrubEnd?.();
          }}
          onBlur={(e) => {
            if (!pointerSeekingRef.current || pendingSeekRef.current === null) return;
            const value = clampSeek(pendingSeekRef.current ?? parseFloat(e.currentTarget.value));
            pointerSeekingRef.current = false;
            seekingRef.current = false;
            pendingSeekRef.current = null;
            props.onScrubEnd?.();
            props.onSeek(value);
          }}
          onChange={(e) => {
            // 键盘改变 range 时没有 pointer 生命周期，change 每次只提交一个目标。
            if (!pointerSeekingRef.current) props.onSeek(clampSeek(parseFloat(e.currentTarget.value)));
          }}
          onInput={(e) => {
            const raw = parseFloat(e.currentTarget.value);
            const value = clampSeek(raw);
            // 若被 clamp（拖入未落盘区域），把滑块视觉也拉回上界，避免手柄越过实际可 seek 边界。
            if (value !== raw) e.currentTarget.value = String(value);
            pendingSeekRef.current = value;
            if (pointerSeekingRef.current) seekingRef.current = true;
            if (curLabelRef.current) curLabelRef.current.textContent = fmt(value);
            const dur = durationRef.current;
            if (pctLabelRef.current) {
              pctLabelRef.current.textContent = `${Math.round((dur > 0 ? value / dur : 0) * 100)}%`;
            }
            if (fillRef.current) {
              fillRef.current.style.transform = `scaleX(${dur > 0 ? Math.min(1, Math.max(0, value / dur)) : 0})`;
            }
            // 仅提交预览目标，不在输入事件栈中同步重建 Three.js 场景。
            scrubNow(value);
          }}
        />
        <div
          className="buffer-track"
          ref={bufferTrackRef}
          title="内存缓存进度"
        >
          <div
            className="buffer-fill"
            ref={bufferFillRef}
            style={{ transform: "scaleX(0)" }}
          />
          <div className="progress-fill" ref={fillRef} />
        </div>
      </div>

      <span className="time-label">{fmt(duration)}</span>
      <span className="progress-pct" ref={pctLabelRef}>0%</span>
      <span className="cache-pct" ref={cacheLabelRef}>缓存 0%</span>

      <span className="speed-label">{speed}×</span>
      <select
        className="speed-select"
        value={speed}
        onChange={(e) => props.onSpeed(parseFloat(e.target.value))}
        title="播放倍速"
      >
        {SPEEDS.map((s) => (
          <option key={s} value={s}>
            {s}×
          </option>
        ))}
      </select>
    </div>
  );
}