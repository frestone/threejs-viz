import { useEffect, useRef, useState } from "react";

interface Props {
  src: string | null;
  title: string;
  // 相机通道 id：用于按通道独立持久化位置/大小(多路相机各自记忆)。
  channelId: string;
  // 勾选相机中的序号：用于计算默认不重叠的智能排布位置。
  index: number;
  // 当前帧原始序列号(来自消息 header.seq)；undefined 时不显示。
  seq?: number;
}

interface Pos {
  x: number;
  y: number;
}
interface Size {
  w: number;
  h: number;
}

// 图像窗口智能排布常量：默认沿视口"左侧"从上往下瀑布流竖排,排满一列后向右新起一列。
// 图表窗口默认沿视口"右侧"竖排(见 ChartPanel.defaultLayoutPos),两者天然分居左右互不遮挡。
const CAM_MARGIN = 12; // 距视口左/上边缘外边距
const CAM_TOP = 12; // 顶部起始 y
const CAM_DEF_W = 320; // 默认宽度
const CAM_ROW_GAP = 10; // 竖排相邻窗口间距
const CAM_COL_GAP = 10; // 多列时列间水平间距
// 依据勾选序号计算默认位置:左侧竖排,按 16:9 估算行高排满一列后向右换列。
function defaultCamPos(index: number): Pos {
  const rowH = CAM_DEF_W * 0.5625 + 26 + CAM_ROW_GAP; // 画面高 + 标题栏 + 间距
  const usableH = Math.max(rowH, window.innerHeight - CAM_TOP - CAM_MARGIN);
  const perCol = Math.max(1, Math.floor(usableH / rowH));
  const col = Math.floor(index / perCol);
  const row = index % perCol;
  return {
    x: CAM_MARGIN + col * (CAM_DEF_W + CAM_COL_GAP),
    y: CAM_TOP + row * rowH,
  };
}

// 浮于视口之上的可拖动 + 可缩放相机画面。位置/大小按通道 id 存 sessionStorage，刷新保留。
export function CameraOverlay({ src, title, channelId, index, seq }: Props) {
  const ref = useRef<HTMLDivElement>(null);
  const imgRef = useRef<HTMLImageElement>(null);
  const posKey = `cam.pos.${channelId}`;
  const sizeKey = `cam.size.${channelId}`;

  const [pos, setPos] = useState<Pos>(() => {
    try {
      const s = sessionStorage.getItem(posKey);
      if (s) return JSON.parse(s);
    } catch {}
    return defaultCamPos(index); // 无记忆时按序号智能排布,避免多路相机重叠
  });
  const [size, setSize] = useState<Size>(() => {
    try {
      const s = sessionStorage.getItem(sizeKey);
      if (s) return JSON.parse(s);
    } catch {}
    return { w: CAM_DEF_W, h: 0 }; // h:0 表示按 img 自身比例自动算
  });
  const dragRef = useRef<{ startX: number; startY: number; baseX: number; baseY: number } | null>(null);
  const resizeRef = useRef<{ startX: number; startY: number; baseW: number; baseH: number } | null>(null);

  useEffect(() => {
    sessionStorage.setItem(posKey, JSON.stringify(pos));
  }, [pos]);
  useEffect(() => {
    sessionStorage.setItem(sizeKey, JSON.stringify(size));
  }, [size]);

  if (!src) return null;

  // 按 img 真实高度自动定高（避免每帧 setState 把高度写死）
  const autoHeight = (e: React.SyntheticEvent<HTMLImageElement>) => {
    const img = e.currentTarget;
    if (img.naturalWidth && size.h === 0) {
      setSize((s) => ({ ...s, h: Math.round((size.w * img.naturalHeight) / img.naturalWidth) }));
    }
  };

  const onTitleMouseDown = (e: React.MouseEvent) => {
    e.preventDefault();
    dragRef.current = { startX: e.clientX, startY: e.clientY, baseX: pos.x, baseY: pos.y };
    window.addEventListener("mousemove", onDragMove);
    window.addEventListener("mouseup", onDragEnd);
  };
  const onDragMove = (e: MouseEvent) => {
    const d = dragRef.current;
    if (!d) return;
    setPos({ x: d.baseX + (e.clientX - d.startX), y: d.baseY + (e.clientY - d.startY) });
  };
  const onDragEnd = () => {
    dragRef.current = null;
    window.removeEventListener("mousemove", onDragMove);
    window.removeEventListener("mouseup", onDragEnd);
  };

  const onResizeMouseDown = (e: React.MouseEvent) => {
    e.preventDefault();
    e.stopPropagation();
    const h0 = ref.current?.clientHeight ?? size.w * 0.5625;
    resizeRef.current = { startX: e.clientX, startY: e.clientY, baseW: size.w, baseH: h0 };
    window.addEventListener("mousemove", onResizeMove);
    window.addEventListener("mouseup", onResizeEnd);
  };
  const onResizeMove = (e: MouseEvent) => {
    const d = resizeRef.current;
    if (!d) return;
    const newW = Math.max(120, d.baseW + (e.clientX - d.startX));
    const img = imgRef.current;
    const ratio = img && img.naturalHeight ? img.naturalHeight / img.naturalWidth : 9 / 16;
    setSize({ w: newW, h: Math.round(newW * ratio) });
  };
  const onResizeEnd = () => {
    resizeRef.current = null;
    window.removeEventListener("mousemove", onResizeMove);
    window.removeEventListener("mouseup", onResizeEnd);
  };

  return (
    <div
      ref={ref}
      className="camera-overlay"
      style={{ left: pos.x, top: pos.y, width: size.w, height: size.h || "auto" }}
    >
      <div className="camera-overlay-title" onMouseDown={onTitleMouseDown}>
        <span>{title}</span>
        {seq !== undefined && seq > 0 && (
          <span className="camera-overlay-seq">#{seq}</span>
        )}
        <button
          className="camera-overlay-reset"
          title="重置位置与大小"
          onMouseDown={(e) => e.stopPropagation()}
          onClick={() => {
            sessionStorage.removeItem(posKey);
            sessionStorage.removeItem(sizeKey);
            setPos(defaultCamPos(index));
            setSize({ w: CAM_DEF_W, h: 0 });
          }}
        >
          ⟳
        </button>
      </div>
      <img
        ref={imgRef}
        src={src}
        alt={title}
        className="camera-overlay-img"
        draggable={false}
        onLoad={autoHeight}
      />
      <div className="camera-overlay-handle" onMouseDown={onResizeMouseDown} />
    </div>
  );
}