import { useCallback, useEffect, useRef, useState } from "react";
import { PlaybackBar } from "./components/PlaybackBar";
import { LayerPanel } from "./components/LayerPanel";
import { ChartPanel } from "./components/ChartPanel";
import { CameraOverlay } from "./components/CameraOverlay";
import { noteBigDataArrival, noteBlobCreated, setDebugEnabled } from "./engine/perfLog";
import { RawDataPanel } from "./components/RawDataPanel";
import { type CameraMode, type ChartData, type ChartDef, type DataMode, type EngineApi, type ImageChannelDef, type LayerDef, type PlaybackStats, type RawDataChannelDef } from "./types";
import { initialPanelState, type PanelState } from "./rawDataPanelState";
import { createEngine, type EngineKind } from "./engine/engine";
import { resolveTransportMode } from "./engine/transport/transportMode";
import { isTauri, pickMcapPath } from "./tauri";
// 图层可见性初始为空——由服务端下发的 LAYER_DEFS（onLayerDefs）动态填充，
// 前端不再硬编码图层列表（唯一来源 decoder*.json 的 layers）。
function defaultVisible(): Record<string, boolean> {
  return {};
}

export default function App() {
  const engineRef = useRef<EngineApi | null>(null);
  const [engineKind, setEngineKind] = useState<EngineKind | "loading">("loading");

  // 相机图像通道：定义由服务端 IMAGE_DEFS 下发（唯一来源 decoder*.json 的 imageChannels），
  // 前端不硬编码。cameraOn=已勾选/已订阅的通道集合（默认空，零成本）；
  // cameraUrls=各通道当前帧 JPEG 的 Blob URL（切帧时 revoke 旧 URL 防泄漏）。
  const [imageDefs, setImageDefs] = useState<ImageChannelDef[]>([]);
  const imageDefsRef = useRef<ImageChannelDef[]>([]);
  const autoImageRequestedRef = useRef(false);
  const [cameraOn, setCameraOn] = useState<Set<string>>(() => new Set<string>());
  const [cameraUrls, setCameraUrls] = useState<Record<string, string>>({});
  const cameraUrlsRef = useRef<Record<string, string>>({});
  const pendingImagesRef = useRef<Record<string, { jpeg: Uint8Array; seq?: number }>>({});
  const imageUpdateRafRef = useRef(0);
  // 各通道当前帧的原始消息 seq(来自消息 header.seq)，叠加显示在相机面板用于诊断。
  const [cameraSeqs, setCameraSeqs] = useState<Record<string, number>>({});

  // 【RawData 面板】通道定义（服务端 type-12 下发，唯一来源 decoder*.json 的 rawData）与
  // 面板状态（由引擎内 panelReducer 演进，经 onRawDataPanelUpdate 推送）。订阅默认关闭。
  const [rawDefs, setRawDefs] = useState<RawDataChannelDef[]>([]);
  const [rawPanel, setRawPanel] = useState<PanelState>(initialPanelState);

  // 【缩略图预览】进度条拖动时保存完整条目，确保 blobUrl 与源图像 seq 同源。
  // 条目由 thumbnailStore 拥有，此处仅引用不 revoke；拖动结束后清空并恢复高清状态。
  const [thumbEntries, setThumbEntries] = useState<
    Record<string, { tSec: number; blobUrl: string; seq: number }>
  >({});
  // 拖动态标记：拖动中高清 onBigDataUpdate 不应覆盖 UI（缩略图优先跟手）。
  const scrubbingRef = useRef(false);
  const [, setScrubbing] = useState(false);

  // UI 状态（由 rAF 轮询从引擎回读）。
  const [time, setTime] = useState(0);
  const [duration, setDuration] = useState(0);
  const [frameCount, setFrameCount] = useState(0);
  const [paused, setPaused] = useState(true);
  const [speed, setSpeed] = useState(1);
  const [visible, setVisible] = useState<Record<string, boolean>>(defaultVisible);
  const [localFile, setLocalFile] = useState<File | null>(null);
  const [fileName, setFileName] = useState("");
  const [uploadProgress, setUploadProgress] = useState(0);
  const [uploading, setUploading] = useState(false);
  const [status, setStatus] = useState("正在连接数据服务");
  const [statusError, setStatusError] = useState(false);
  const [charts, setCharts] = useState<ChartData[]>([]);
  // 相机模式：follow=跟随主车（默认）；free=自由浏览（俯视、可鼠标拖拽平移）。
  const [cameraMode, setCameraModeState] = useState<CameraMode>("follow");
  // 数据包运行模式（高精/轻图），由服务端 StreamInfo 下发；null=未加载。
  const [dataMode, setDataMode] = useState<DataMode>(null);
  // 播放性能指标（渲染帧率/缓存深度/缓冲余量），用于诊断播放卡顿；仅 three 引擎提供。
  const [stats, setStats] = useState<PlaybackStats | null>(null);
  // 图表定义与显隐控制：定义由服务端连接建立时下发（唯一来源 configs/decoder.json
  // 的 charts），前端不硬编码；初始隐藏集合按下发配置的 visible=false 计算。
  const [chartDefs, setChartDefs] = useState<ChartDef[]>([]);
  const [chartHidden, setChartHidden] = useState<Set<string>>(() => new Set<string>());

  // 图层定义：由服务端连接建立时下发（唯一来源 configs/decoder*.json 的 layers），
  // 前端不硬编码图层列表——高精/轻图各自的图层（含轻图 road/lane/crosswalk 等）据此
  // 动态生成显隐复选框。新下发的图层默认可见并同步到引擎。
  const [layerDefs, setLayerDefs] = useState<LayerDef[]>([]);
  // 显示策略由传输架构决定：Desktop FFI 本机无网络瓶颈，原图优先；
  // Web/WS 跨机部署带宽有限，以全量缩略图缓存为中心显示。
  const [imageDisplayMode] = useState<"original" | "thumbnail">(
    () => (isTauri() && !new URLSearchParams(window.location.search).get("ws")
      ? "original"
      : "thumbnail"),
  );

  const onLayerDefs = (defs: LayerDef[]) => {
    setLayerDefs(defs);
    setVisible((prev) => {
      const next = { ...prev };
      defs.forEach((d) => {
        // 首次出现的图层按后端 visible 初始化；已存在的保留用户当前选择。
        if (!(d.id in next)) next[d.id] = d.visible !== false;
      });
      return next;
    });
    // 同步初始可见性到引擎（新图层）。
    defs.forEach((d) => engineRef.current?.setLayerVisible(d.id, d.visible !== false));
  };

  // 接收服务端下发的图表定义（唯一来源 decoder.json 的 charts）：
  // 更新侧栏按钮列表，并按 visible=false 初始化隐藏集合。
  const onChartDefs = (defs: ChartDef[]) => {
    setChartDefs(defs);
    setChartHidden(new Set(defs.filter((d) => d.visible === false).map((d) => d.id)));
  };

  // 接收服务端下发的相机图像通道定义（唯一来源 decoder*.json 的 imageChannels）：
  // 更新图像组的相机复选框列表；默认全部不勾选、不订阅（零成本）。
  const onImageDefs = (defs: ImageChannelDef[]) => {
    imageDefsRef.current = defs;
    setImageDefs(defs);
    trySubscribeFirstImage();
  };

  // 是否运行在 Tauri 桌面外壳中（决定是否启用原生文件对话框直传）。
  const desktop = isTauri();

  // 默认订阅第一路相机。IMAGE_DEFS 可能早于/晚于 engineRef 挂载，因此在这两个
  // 时点都尝试一次；autoImageRequestedRef 保证整个应用生命周期只自动订阅一次。
  const trySubscribeFirstImage = () => {
    if (autoImageRequestedRef.current) return;
    const firstChannel = imageDefsRef.current[0];
    const engine = engineRef.current;
    if (!firstChannel || !engine) return;
    autoImageRequestedRef.current = true;
    setCameraOn((prev) => new Set(prev).add(firstChannel.id));
    engine.subscribeImage?.(firstChannel.id, true);
  };

  const canvasRef = useRef<HTMLCanvasElement | null>(null);

  // Debug 模式解析（仅开启时输出性能诊断，打包默认关闭）：
  //   桌面端以 Tauri 侧 ffi_is_debug 为准（debug 构建 / VIZ_DEBUG / --debug）；
  //   浏览器端 ?debug=1 开启；dev 构建默认开启，vite build 打包自动关闭。
  useEffect(() => {
    let cancelled = false;
    (async () => {
      let enabled = import.meta.env.DEV;
      if (isTauri()) {
        try {
          const { invoke } = await import("@tauri-apps/api/core");
          if (!cancelled) enabled = enabled || (await invoke<boolean>("ffi_is_debug"));
        } catch {
          // 外壳命令不可用时仅保留 dev 默认值。
        }
      } else {
        enabled =
          enabled || new URLSearchParams(window.location.search).get("debug") === "1";
      }
      if (!cancelled) setDebugEnabled(enabled);
    })();
    return () => {
      cancelled = true;
    };
  }, []);

  // 初始化引擎：优先 Three.js 渲染，失败回退 stream，再回退 mock。
  useEffect(() => {
    let cancelled = false;
    (async () => {
      const canvas = canvasRef.current!;
      const preferThumbnails = imageDisplayMode === "thumbnail";
      const onStatus = (message: string, error = false) => {
        setStatus(message);
        setStatusError(error);
        (globalThis as unknown as { __vizStatus?: string }).__vizStatus = message;
        if (error || message.startsWith("已加载")) setUploading(false);
      };
      // 每个动画帧只提交各通道最新 JPEG，避免 Blob/URL/React 更新在主线程积压。
      const queueImageUpdate = (channel: string, jpeg: Uint8Array, seq?: number) => {
        pendingImagesRef.current[channel] = { jpeg, seq };
        if (imageUpdateRafRef.current) return;
        imageUpdateRafRef.current = requestAnimationFrame(() => {
          imageUpdateRafRef.current = 0;
          const pending = pendingImagesRef.current;
          pendingImagesRef.current = {};
          const nextUrls = { ...cameraUrlsRef.current };
          const seqs: Record<string, number> = {};
          for (const [pendingChannel, image] of Object.entries(pending)) {
            const bytes = new Uint8Array(image.jpeg);
            const url = URL.createObjectURL(new Blob([bytes.buffer], { type: "image/jpeg" }));
            if (image.seq !== undefined) noteBlobCreated(pendingChannel, image.seq);
            const prev = nextUrls[pendingChannel];
            if (prev) URL.revokeObjectURL(prev);
            nextUrls[pendingChannel] = url;
            if (image.seq !== undefined) seqs[pendingChannel] = image.seq;
          }
          cameraUrlsRef.current = nextUrls;
          setCameraUrls(nextUrls);
          if (Object.keys(seqs).length) setCameraSeqs((current) => ({ ...current, ...seqs }));
        });
      };
      const onImage = (channel: string, jpeg: Uint8Array, _w: number, _h: number) => {
        if (preferThumbnails) return; // 分离部署不在前端消费/解码原图，节省主线程与带宽。
        queueImageUpdate(channel, jpeg);
      };
      // 【大数据流】某 channel 的当前大数据帧被替换时回调：image 从引擎持有者取当前帧字节。
      const onBigDataUpdate = (channel: string, kind: "image" | "raw" | "thumbnail") => {
        if (kind === "thumbnail") {
          if (!preferThumbnails) return;
          // 缩略图是 Web 模式的正常播放源，不只是拖动预览源。后台铺底每产出一帧，
          // 就按当前引擎 playhead 就近取用，避免等待一个完整的原图才刷新。
          const engine = engineRef.current;
          const entry = engine?.getThumbnailEntry?.(channel, engine.getTime());
          if (!entry) return;
          setThumbEntries((entries) => {
            const current = entries[channel];
            if (current?.seq === entry.seq) return entries;
            return { ...entries, [channel]: entry };
          });
          return;
        }
        if (preferThumbnails && kind === "image") return;
        if (kind !== "image") return;
        const cur = engineRef.current?.getCurrentBigData?.(channel);
        if (!cur) return;
        noteBigDataArrival(channel, cur.seq);
        queueImageUpdate(channel, cur.payload, cur.seq);
        // 拖动中保留缩略图；松手后等高清时间追上缩略图时间再切回。
        setThumbEntries((entries) => {
          const thumb = entries[channel];
          if (!thumb) return entries;
          // 拖动中缩略图必须保持跟手。松手后不再要求 seq 严格相等：HEVC 从 I 帧
          // 重同步、图像丢帧或后续 seek 都可能跳过那一张源帧；只要高清时间已追上
          // 缩略图对应时间，就切回高清。否则旧缩略图会永久盖住正在播放的相机。
          if (scrubbingRef.current || cur.tSec + 1e-3 < thumb.tSec) return entries;
          const next = { ...entries };
          delete next[channel];
          return next;
        });
      };
      // 【RawData 面板】通道定义下发：存 rawData 通道列表供面板渲染（默认全部不选、不订阅）。
      const onRawDataDefs = (defs: import("./types").RawDataDefs) => {
        setRawDefs(defs.rawData);
      };
      // 面板状态推送：引擎内 panelReducer 演进后回调，前端仅镜像存储（单一真相源在引擎）。
      const onRawDataPanelUpdate = (s: PanelState) => {
        setRawPanel(s);
      };
      // 桌面默认使用进程内 FFI；Web 保留 WebSocket 及 ?ws= 调试覆盖。
      const wsOverride = new URLSearchParams(window.location.search).get("ws");
      const transportMode = resolveTransportMode(desktop, wsOverride ? "ws" : undefined);
      const wsUrl = wsOverride ?? "/ws";
      let result: { engine: EngineApi; kind: EngineKind };
      try {
        result = await createEngine("three", { canvas, wsUrl, transportMode, onStatus, onChartDefs, onLayerDefs, onImageDefs, onImage, onBigDataUpdate, onRawDataDefs, onRawDataPanelUpdate });
      } catch (threeError) {
        onStatus(
          threeError instanceof Error
            ? `Three.js 渲染器加载失败，回退数据流：${threeError.message}`
            : "Three.js 渲染器加载失败，回退数据流",
          true
        );
        try {
          result = await createEngine("stream", { canvas, wsUrl, onStatus, onChartDefs, onLayerDefs });
        } catch {
          result = await createEngine("mock", { canvas });
        }
      }
      if (cancelled) {
        result.engine.dispose();
        return;
      }
      engineRef.current = result.engine;
      setEngineKind(result.kind);
      trySubscribeFirstImage();
      // 同步初始可见性到引擎。
      Object.entries(defaultVisible()).forEach(([id, v]) =>
        result.engine.setLayerVisible(id, v)
      );
    })();
    return () => {
      cancelled = true;
      if (imageUpdateRafRef.current) cancelAnimationFrame(imageUpdateRafRef.current);
      imageUpdateRafRef.current = 0;
      pendingImagesRef.current = {};
      Object.values(cameraUrlsRef.current).forEach((url) => URL.revokeObjectURL(url));
      cameraUrlsRef.current = {};
      engineRef.current?.dispose();
      engineRef.current = null;
    };
  }, []);

  // rAF 轮询引擎状态刷新 UI。
  // 【进度条卡顿修复】区分「高频轻量」与「低频重量」两类状态,避免每 rAF 都整树重渲染:
  //   - time/duration/paused/speed/frameCount 是标量,每 rAF 更新;值不变时 React 自动跳过重渲染,
  //     进度条 time 得以高频平滑推进。
  //   - stats/charts/dataMode 每次 getStats()/getCharts() 都返回新对象引用,若每 rAF setState 会
  //     强制整个 App 重渲染,与几何 build/GPU 绘制争抢主线程→重渲染被推迟→进度条一顿一顿。
  //     这类降频到每 ~150ms 刷新一次即可(帧率/缓存等诊断指标无需 60Hz)。
  useEffect(() => {
    let raf = 0;
    let lastHeavy = 0;
    const HEAVY_INTERVAL_MS = 150;
    const loop = () => {
      const e = engineRef.current;
      if (e) {
        // 高频轻量:进度条/时间/播放态,每 rAF 更新(相同值 React 会跳过重渲染)。
        setTime(e.getTime());
        setDuration(e.getDuration());
        setFrameCount(e.getFrameCount());
        setPaused(e.isPaused());
        setSpeed(e.getSpeed());
        // 低频重量:图表/统计/数据模式,降频刷新避免每帧强制整树重渲染拖累主线程。
        const now = performance.now();
        if (now - lastHeavy >= HEAVY_INTERVAL_MS) {
          lastHeavy = now;
          if (e.getCharts) setCharts(e.getCharts());
          if (e.getDataMode) setDataMode(e.getDataMode());
          if (e.getStats) setStats(e.getStats());
        }
      }
      raf = requestAnimationFrame(loop);
    };
    raf = requestAnimationFrame(loop);
    return () => cancelAnimationFrame(raf);
  }, []);

  // 注：相机交互由 Three.js OrbitControls 在 canvas 上自行处理，
  // 无需在 App 层手动绑定 orbit/pan/dolly 事件。

  const onTogglePause = () => {
    const e = engineRef.current;
    if (!e) return;
    const next = !paused;
    // 乐观更新：点击即切换 UI 播放态，不等下一个 rAF 从 e.isPaused() 回读。
    // rAF loop 繁忙(几何 build/GPU 绘制)时会被主线程推迟，若只靠 rAF 回读，按钮
    // 图标要拖到下一帧才变，表现为「点了没反应」。此处与 engine.setPaused 同步置位，
    // rAF 回读只作后续对齐(值一致时 React 跳过重渲染)。
    setPaused(next);
    e.setPaused(next);
  };
  const onSeek = (t: number) => {
    setTime(t);
    engineRef.current?.seek(t);
  };
  // 拖动输入只投递最新预览目标；引擎在下一渲染周期按帧索引定位并至多构建一帧。
  const onScrub = (t: number) => {
    engineRef.current?.requestPreview?.(t);
    // 【缩略图预览】拖动时对每个已开启的相机通道，查最接近 t 的缩略图 blobUrl，
    // 命中即刷新低清预览（跟手），未生成区域保持上一张/空白。缩略图 blobUrl 由
    // thumbnailStore 拥有，此处仅引用不 revoke。
    const e = engineRef.current;
    if (!e?.getThumbnailEntry) return;
    setThumbEntries((prev) => {
      let changed = false;
      const next = { ...prev };
      cameraOn.forEach((channel) => {
        const entry = e.getThumbnailEntry!(channel, t);
        if (entry && (entry.blobUrl !== next[channel]?.blobUrl || entry.seq !== next[channel]?.seq)) {
          next[channel] = entry;
          changed = true;
        }
      });
      return changed ? next : prev;
    });
  };
  const onScrubStart = () => {
    scrubbingRef.current = true;
    setScrubbing(true);
    engineRef.current?.setScrubbing?.(true);
  };
  // 先结束预览态，随后 PlaybackBar 用 onSeek 正式提交最终位置。
  // 缩略图是否切换由 onBigDataUpdate 的高清时间判断，避免源序号被 seek 跳过。
  const onScrubEnd = () => {
    scrubbingRef.current = false;
    setScrubbing(false);
    engineRef.current?.setScrubbing?.(false);
  };
  const onSpeed = (s: number) => engineRef.current?.setSpeed(s);
  // 进度条自驱动用的实时时钟读取器：引用恒定（读 engineRef），
  // 避免作为 PlaybackBar prop 变化导致其内部 rAF effect 反复重建。
  const liveTimeGetter = useCallback(() => engineRef.current?.getTime() ?? 0, []);
  // 已加载(缓存)边界读取器：进度条 CSS 平滑动画只推进到该边界，避免回填未跟上时越过场景画面。
  const loadedTimeGetter = useCallback(
    () => engineRef.current?.getLoadedTime?.() ?? engineRef.current?.getDuration?.() ?? 0,
    [],
  );
  // 可拖动上界读取器：方案A纯内存,拖动上界=已解码进内存的最大帧时刻(getSeekableMax=loadedMaxTime),随缓存增长放开。
  const seekableMaxGetter = useCallback(
    () => engineRef.current?.getSeekableMax?.() ?? 0,
    [],
  );
  // 切换相机模式：更新本地状态并下发引擎（引擎无该能力时静默忽略）。
  const onCameraMode = (mode: CameraMode) => {
    setCameraModeState(mode);
    engineRef.current?.setCameraMode?.(mode);
  };
  // 切换某相机通道的订阅：勾选后请求后端解码 HEVC 下发；取消时清理该通道图像与 Blob URL。
  const onToggleCamera = (channel: string, enabled: boolean) => {
    setCameraOn((prev) => {
      const next = new Set(prev);
      if (enabled) next.add(channel);
      else next.delete(channel);
      return next;
    });
    engineRef.current?.subscribeImage?.(channel, enabled);
    if (!enabled) {
      delete pendingImagesRef.current[channel];
      const url = cameraUrlsRef.current[channel];
      if (url) URL.revokeObjectURL(url);
      const nextRef = { ...cameraUrlsRef.current };
      delete nextRef[channel];
      cameraUrlsRef.current = nextRef;
      setCameraUrls((m) => {
        const n = { ...m };
        delete n[channel];
        return n;
      });
      setThumbEntries((m) => {
        const n = { ...m };
        delete n[channel];
        return n;
      });
      setCameraSeqs((m) => {
        const n = { ...m };
        delete n[channel];
        return n;
      });
    }
  };
  const onToggleLayer = (id: string, v: boolean) => {
    setVisible((prev) => ({ ...prev, [id]: v }));
    engineRef.current?.setLayerVisible(id, v);
  };
  // 图表显隐：checked=显示（从隐藏集合移除），unchecked=隐藏（加入集合）。
  const onToggleChart = (id: string, show: boolean) => {
    setChartHidden((prev) => {
      const next = new Set(prev);
      if (show) next.delete(id);
      else next.add(id);
      return next;
    });
  };
  const onUploadFile = async () => {
    if (!localFile || !engineRef.current?.uploadFile) return;
    // 换源前清掉旧流显示状态；缩略图 blobUrl 由引擎 thumbnailStore 清理/回收。
    pendingImagesRef.current = {};
    Object.values(cameraUrlsRef.current).forEach((url) => URL.revokeObjectURL(url));
    cameraUrlsRef.current = {};
    setCameraUrls({});
    setThumbEntries({});
    setCameraSeqs({});
    setUploading(true);
    setUploadProgress(0);
    setStatusError(false);
    try {
      await engineRef.current.uploadFile(localFile, setUploadProgress);
    } catch (error) {
      setUploading(false);
      setStatusError(true);
      setStatus(error instanceof Error ? error.message : "上传失败");
    }
  };
  // 统一打开入口：只输入文件名——服务端本地缓存（s3.json cacheDir）命中走本地
  // MCAP reader，未命中则按 record 名走 S3 流式 range 拉取。
  const onOpenFileName = () => {
    const name = fileName.trim();
    if (!name || !engineRef.current?.openByName) return;
    setStatusError(false);
    pendingImagesRef.current = {};
    Object.values(cameraUrlsRef.current).forEach((url) => URL.revokeObjectURL(url));
    cameraUrlsRef.current = {};
    setCameraUrls({});
    setThumbEntries({});
    setCameraSeqs({});
    engineRef.current.openByName(name);
  };
  // Tauri 桌面：原生对话框选 .mcap，取文件名填入输入框并直接经统一入口打开
  // （服务端对绝对路径保持本地直开语义）。
  const onBrowseLocalPath = async () => {
    try {
      const picked = await pickMcapPath();
      if (!picked) return;
      setFileName(picked);
      setStatusError(false);
      pendingImagesRef.current = {};
      Object.values(cameraUrlsRef.current).forEach((url) => URL.revokeObjectURL(url));
      cameraUrlsRef.current = {};
      setCameraUrls({});
      setThumbEntries({});
      setCameraSeqs({});
      engineRef.current?.openByName?.(picked);
    } catch (error) {
      setStatusError(true);
      setStatus(error instanceof Error ? error.message : "选择文件失败");
    }
  };
  const formatBytes = (value: number) => {
    if (value < 1024 * 1024) return `${(value / 1024).toFixed(1)} KiB`;
    return `${(value / (1024 * 1024)).toFixed(1)} MiB`;
  };

  return (
    <div className="app">
      <aside className="sidebar">
        <h2>Three.js Viz</h2>
        <div className="engine-badge">
          引擎：{engineKind === "loading" ? "加载中…" : engineKind === "mock" ? "Mock（无后端）" : engineKind === "stream" ? "原生数据流（渲染待接入）" : "Three.js 渲染"}
        </div>
        <section className="file-browser">
          <h3>MCAP 数据</h3>
          <div className="local-path">
            <input
              type="text"
              placeholder="输入文件名：本地缓存存在则直接读取，否则经 S3 流式拉取"
              value={fileName}
              disabled={uploading}
              onChange={(event) => setFileName(event.target.value)}
              onKeyDown={(event) => {
                if (event.key === "Enter") onOpenFileName();
              }}
            />
            <button type="button" onClick={onOpenFileName} disabled={!fileName.trim() || uploading}>
              打开
            </button>
            {desktop && (
              <button type="button" onClick={onBrowseLocalPath} disabled={uploading}>
                浏览…
              </button>
            )}
          </div>
          {desktop && (
            <div className="upload-divider">桌面模式：可浏览选择本地文件（绝对路径直开）</div>
          )}
          {!desktop && (
          <>
          <div className="upload-divider">或上传浏览器本地文件</div>
          <input
            type="file"
            accept=".mcap"
            disabled={uploading}
            onChange={(event) => {
              setLocalFile(event.target.files?.[0] ?? null);
              setUploadProgress(0);
            }}
          />
          {localFile && <div className="file-status">{localFile.name}（{formatBytes(localFile.size)}）</div>}
          <div className="file-actions">
            <button type="button" onClick={onUploadFile} disabled={!localFile || uploading}>
              {uploading ? `上传中 ${Math.round(uploadProgress * 100)}%` : "上传并打开"}
            </button>
          </div>
          {uploading && <progress value={uploadProgress} max={1} />}
          </>
          )}
          <div className={statusError ? "file-status error" : "file-status"}>{status}</div>
        </section>
        <LayerPanel
          visible={visible}
          onToggle={onToggleLayer}
          layerDefs={layerDefs}
          imageDefs={imageDefs}
          cameraOn={cameraOn}
          onToggleCamera={onToggleCamera}
        />
        {chartDefs.length > 0 && (
          <div className="layer-panel">
            <h3>图表</h3>
            <ul className="layer-tree">
              {chartDefs.map((d) => (
                <li key={d.id}>
                  <label className="layer-row">
                    <input
                      type="checkbox"
                      checked={!chartHidden.has(d.id)}
                      onChange={(e) => onToggleChart(d.id, e.target.checked)}
                    />
                    <span>{d.title || d.id}</span>
                  </label>
                </li>
              ))}
            </ul>
          </div>
        )}
        <ChartPanel chartDefs={chartDefs} charts={charts} hidden={chartHidden} />
        <div className="camera-panel">
          <div className="camera-title">相机模式</div>
          <div className="camera-modes">
            <button
              type="button"
              className={cameraMode === "follow" ? "cam-btn active" : "cam-btn"}
              onClick={() => onCameraMode("follow")}
              title="相机跟随主车移动"
            >
              跟随
            </button>
            <button
              type="button"
              className={cameraMode === "free" ? "cam-btn active" : "cam-btn"}
              onClick={() => onCameraMode("free")}
              title="俯视自由浏览，鼠标拖拽平移地图与渲染对象"
            >
              自由浏览
            </button>
          </div>
        </div>
        <div className="stats">
          <div>帧数：{frameCount}</div>
          <div>时长：{duration.toFixed(1)}s</div>
          <div>
            数据模式：
            <span className={`data-mode ${dataMode ?? "unknown"}`}>
              {dataMode === "highprec" ? "高精" : dataMode === "lightmap" ? "轻图" : "未知"}
            </span>
          </div>
        </div>
        {stats && (
          <div className="perf-panel">
            <div className="perf-title">播放性能</div>
            <div className="perf-row">
              <span>渲染帧率</span>
              <span className={stats.renderFps < 8 && !paused ? "perf-warn" : ""}>
                {stats.renderFps.toFixed(1)} Hz
           </span>
            </div>
            <div className="perf-row">
              <span>rAF 帧率</span>
              <span>{stats.fps.toFixed(1)} Hz</span>
            </div>
            <div className="perf-row">
              <span>几何耗时</span>
              <span className={stats.frameBuildMs > 16 ? "perf-warn" : ""}>
                {stats.frameBuildMs.toFixed(1)} ms
              </span>
            </div>
            <div className="perf-row">
              <span>GPU 耗时</span>
              <span className={stats.gpuRenderMs > 16 ? "perf-warn" : ""}>
                {stats.gpuRenderMs.toFixed(1)} ms
              </span>
            </div>
            <div className="perf-row">
              <span>缓存帧数</span>
              <span>{stats.cacheFrames}</span>
            </div>
            <div className="perf-row">
              <span>缓存内存</span>
              <span className={stats.cacheBytesMB > 1400 ? "perf-warn" : ""}>
                {stats.cacheBytesMB} MB
              </span>
            </div>
            <div className="perf-row">
              <span>缓冲余量</span>
              <span className={stats.bufferAheadSec < 0.2 && !paused ? "perf-warn" : ""}>
                {stats.bufferAheadSec.toFixed(2)}s
              </span>
            </div>
            {stats.starving && (
              <div className="perf-row perf-alert">后端供数跟不上（卡顿主因）</div>
            )}
          </div>
        )}
      </aside>
      <main className="viewport">
        <canvas ref={canvasRef} className="viz-canvas" />
        {imageDefs
          .filter((d) => cameraOn.has(d.id))
          .map((d, i) => (
            <CameraOverlay
              key={d.id}
              channelId={d.id}
              index={i}
              src={
                imageDisplayMode === "thumbnail"
                  ? thumbEntries[d.id]?.blobUrl ?? cameraUrls[d.id]
                  : cameraUrls[d.id] ?? thumbEntries[d.id]?.blobUrl
              }
              title={d.label || d.id}
              seq={
                imageDisplayMode === "thumbnail"
                  ? thumbEntries[d.id]?.seq ?? cameraSeqs[d.id]
                  : cameraSeqs[d.id] ?? thumbEntries[d.id]?.seq
              }
            />
          ))}
      </main>

      <RawDataPanel
        state={rawPanel}
        defs={rawDefs}
        onSelectChannel={(channel) =>
          engineRef.current?.subscribeRawData?.(channel ?? rawPanel.selected ?? "", channel !== null)
        }
        onVisibilityChange={(visible) => engineRef.current?.setRawDataPanelVisible?.(visible)}
        onQuery={(query) => engineRef.current?.dispatchRawDataPanelAction?.({ type: "query", query })}
        onNextMatch={() => engineRef.current?.dispatchRawDataPanelAction?.({ type: "nextMatch" })}
        onPrevMatch={() => engineRef.current?.dispatchRawDataPanelAction?.({ type: "previousMatch" })}
        onPageChange={(page) => engineRef.current?.dispatchRawDataPanelAction?.({ type: "setPage", page })}
        onViewport={(scrollTop, viewportHeight, rowHeight) =>
          engineRef.current?.dispatchRawDataPanelAction?.({
            type: "setViewport",
            scrollTop,
            viewportHeight,
            rowHeight,
          })
        }
      />

      <footer className="footer">
        <PlaybackBar
          time={time}
          duration={duration}
          paused={paused}
          speed={speed}
          getLiveTime={liveTimeGetter}
          getLoadedTime={loadedTimeGetter}
          getSeekableMax={seekableMaxGetter}
          onTogglePause={onTogglePause}
          onSeek={onSeek}
          onScrub={onScrub}
          onScrubStart={onScrubStart}
          onScrubEnd={onScrubEnd}
          onSpeed={onSpeed}
        />
      </footer>
    </div>
  );
}
