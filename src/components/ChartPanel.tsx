import { useCallback, useEffect, useMemo, useRef, useState } from "react";
import * as echarts from "echarts";
import type { ChartData, ChartDef, ChartSeriesData } from "../types";

export interface ChartPanelProps {
  // 图表配置定义(来自 configs/decoder.json 的 charts):决定有几个弹出对话框,
  // 与是否有帧数据无关。
  chartDefs: ChartDef[];
  // 当前帧图表数据（由引擎 getCharts() 提供）。
  charts: ChartData[];
  // 被隐藏的图表 id 集合（受控，由主页面控制区管理）。
  // hidden.has(id) => 该图表的整个弹出对话框不渲染。
  hidden: Set<string>;
}

// 单个浮动对话框宽度（含内边距）。
const PANEL_WIDTH = 320;
const CHART_HEIGHT = 200;

// 把一个 series 的 x/y 两个平行数组压成 ECharts 需要的 [x, y] 数据对。
function toPairs(s: ChartSeriesData): [number, number][] {
  const n = Math.min(s.x.length, s.y.length);
  const out: [number, number][] = new Array(n);
  for (let i = 0; i < n; i++) out[i] = [s.x[i], s.y[i]];
  return out;
}

// 依据 ChartData 生成 ECharts option（含 tooltip 十字准线、图例、坐标轴刻度）。
// 标题由对话框标题栏显示，ECharts 内不再画 title。
function buildOption(chart: ChartData): echarts.EChartsOption {
  const legendData: string[] = [];
  const series: echarts.SeriesOption[] = [];
  for (const s of chart.series) {
    // band 上下界作为普通线绘制，避免额外区域填充逻辑。
    const isScatter = s.kind === "scatter";
    legendData.push(s.name);
    series.push({
      name: s.name,
      type:isScatter ? "scatter" : "line",
      showSymbol: isScatter,
      symbolSize: isScatter ? 4 : 0,
      lineStyle: { width: s.kind === "line" ? 1.5 : 0.8, color: s.color },
      itemStyle: { color: s.color },
      data: toPairs(s),
      smooth: false,
    });
  }
  return {
    animation: false,
    backgroundColor: "transparent",
    grid: { left: 46, right: 12, top: 24, bottom: 34, containLabel: false },
    legend: {
      data: legendData,
      top: 4,
      right: 8,
      textStyle: { color: "#8b949e", fontSize: 10 },
      itemWidth: 14,
      itemHeight: 8,
    },
    tooltip: {
      trigger: "axis",
      axisPointer: { type: "cross", label: { backgroundColor: "#30363d" } },
      backgroundColor: "rgba(13,17,23,0.92)",
      borderColor: "#30363d",
      textStyle: { color: "#c9d1d9", fontSize: 11 },
      valueFormatter: (v) => (typeof v === "number" ? v.toFixed(3) : String(v)),
    },
    xAxis: {
      type: "value",
      name: chart.xLabel,
      nameLocation: "middle",
      nameGap: 20,
      nameTextStyle: { color: "#8b949e", fontSize: 10 },
      axisLine: { lineStyle: { color: "#30363d" } },
      axisLabel: { color: "#8b949e", fontSize: 10, hideOverlap: true },
      splitLine: { lineStyle: { color: "rgba(48,54,61,0.5)" } },
      scale: true,
    },
    yAxis: {
      type: "value",
      name: chart.yLabel,
      nameLocation: "middle",
      nameGap: 34,
      nameTextStyle: { color: "#8b949e", fontSize: 10 },
      axisLine: { lineStyle: { color: "#30363d" } },
      axisLabel: { color: "#8b949e", fontSize: 10, hideOverlap: true },
      splitLine: { lineStyle: { color: "rgba(48,54,61,0.5)" } },
      scale: true,
    },
    series,
  };
}

// 单张图表画布宽度前的公共常量上方：图表位置持久缓存（见 FloatingChart）。
// key = chart.id。模块级缓存：组件因隐藏卸载后重新挂载仍能取回上次位置。
const savedPositions = new Map<string, { x: number; y: number }>();

// 图表默认智能布局：沿视口右侧边缘竖排，主动避开中央主 3D 渲染区域。
// 每张对话框高度约 CHART_HEIGHT + 标题栏，按固定行距竖排；排满一屏高度后
// 向左新起一列（列间距 = PANEL_WIDTH + 间隙），保证多图表也不覆盖画布中央。
const MARGIN = 16; // 距视口边缘的外边距
const TOP_OFFSET = 72; // 顶部工具区留白
const ROW_GAP = 12; // 竖排相邻对话框间距
const COL_GAP = 12; // 多列时列间水平间距
function defaultLayoutPos(index: number): { x: number; y: number } {
  const rowHeight = CHART_HEIGHT + 40 + ROW_GAP; // 40 ≈ 标题栏 + body 内边距
  const usableH = Math.max(rowHeight, window.innerHeight - TOP_OFFSET - MARGIN);
  const perColumn = Math.max(1, Math.floor(usableH / rowHeight));
  const col = Math.floor(index / perColumn);
  const row = index % perColumn;
  const x = Math.max(MARGIN, window.innerWidth - PANEL_WIDTH - MARGIN - col * (PANEL_WIDTH + COL_GAP));
  const y = TOP_OFFSET + row * rowHeight;
  return { x, y };
}

// 单张图表画布：用 ECharts 实例渲染，实时数据用 setOption merge 更新避免重建闪烁。
function ChartCanvas({ chart }: { chart: ChartData }) {
  const ref = useRef<HTMLDivElement>(null);
  const instRef = useRef<echarts.ECharts | null>(null);

  // 初始化 / 销毁实例。
  useEffect(() => {
    if (!ref.current) return;
    const inst = echarts.init(ref.current, undefined, { renderer: "canvas" });
    instRef.current = inst;
    const onResize = () => inst.resize();
    window.addEventListener("resize", onResize);
    return () => {
      window.removeEventListener("resize", onResize);
      inst.dispose();
      instRef.current = null;
    };
  }, []);

  // 数据更新：每帧 setOption（notMerge=false 让 ECharts 复用轴/tooltip 交互态）。
  useEffect(() => {
    const inst = instRef.current;
    if (!inst) return;
    inst.setOption(buildOption(chart), { notMerge: false, lazyUpdate: true });
  }, [chart]);

  return <div ref={ref} className="chart-canvas" style={{ width: "100%", height: CHART_HEIGHT }} />;
}

// 单个图表的独立浮动对话框：标题栏可拖拽改变位置。
// 对话框整体显示与否由外层 hidden 控制（hidden.has(id) 时外层直接不渲染本组件）。
function FloatingChart({ chart, index }: { chart: ChartData; index: number }) {
  // 对话框浮动位置（相对视口）。默认智能排布在视口右侧竖列，避开中央主 3D 渲染区；
  // 竖列排满一屏后向左再起一列。用户拖拽过的位置按 chart.id 记入 savedPositions，
  // 隐藏后再次显示时恢复，不回到默认位置。
  const [pos, setPos] = useState<{ x: number; y: number }>(() => {
    const saved = savedPositions.get(chart.id);
    if (saved) return saved;
    return defaultLayoutPos(index);
  });
  const dragRef = useRef<{ dx: number; dy: number } | null>(null);
  const [dragging, setDragging] = useState(false);

  // 标题栏按下：记录鼠标相对对话框左上角的偏移，进入拖拽态。
  const onHandleDown = useCallback(
    (e: React.MouseEvent) => {
      // 点在控制按钮上时不触发拖拽。
      if ((e.target as HTMLElement).closest(".chart-panel-btn")) return;
      e.preventDefault();
      dragRef.current = { dx: e.clientX - pos.x, dy: e.clientY - pos.y };
      setDragging(true);
    },
    [pos.x, pos.y],
  );

  // 拖拽期间在 window 上监听 move/up，避免鼠标移出对话框时丢事件。
  useEffect(() => {
    if (!dragging) return;
    const onMove = (e: MouseEvent) => {
      const d = dragRef.current;
      if (!d) return;
      const maxX = window.innerWidth - PANEL_WIDTH;
      const nx = Math.min(Math.max(0, e.clientX - d.dx), Math.max(0, maxX));
      const ny = Math.min(Math.max(0, e.clientY - d.dy), Math.max(0, window.innerHeight - 40));
      savedPositions.set(chart.id, { x: nx, y: ny });
      setPos({ x: nx, y: ny });
    };
    const onUp = () => {
      dragRef.current = null;
      setDragging(false);
    };
    window.addEventListener("mousemove", onMove);
    window.addEventListener("mouseup", onUp);
    return () => {
      window.removeEventListener("mousemove", onMove);
      window.removeEventListener("mouseup", onUp);
    };
  }, [dragging]);

  return (
    <div className="chart-panel" style={{ left: pos.x, top: pos.y, width: PANEL_WIDTH }}>
      <div
        className={dragging ? "chart-panel-handle dragging" : "chart-panel-handle"}
        onMouseDown={onHandleDown}
      >
        <span>{chart.title || chart.id}</span>
      </div>
      <div className="chart-panel-body">
        <ChartCanvas chart={chart} />
      </div>
    </div>
  );
}

// 图表面板集合：每个图表配置一个独立弹出对话框。
// 显隐语义：hidden.has(id) => 该图表整个对话框隐藏（不渲染）。
export function ChartPanel(props: ChartPanelProps) {
  const { chartDefs, charts, hidden } = props;

  // 当前帧图表数据按 id 建索引，便于按配置顺序取数据。
  const chartById = useMemo(() => {
    const m = new Map<string, ChartData>();
    for (const c of charts) m.set(c.id, c);
    return m;
  }, [charts]);

  // 无任何图表配置时不渲染。
  if (!chartDefs || chartDefs.length === 0) return null;

  // 每个未隐藏且当前有帧数据的图表渲染一个独立浮动对话框。
  const panels: JSX.Element[] = [];
  let visibleIndex = 0;
  for (const def of chartDefs) {
    if (hidden.has(def.id)) continue;
    const chart = chartById.get(def.id);
    if (!chart) continue;
    panels.push(<FloatingChart key={def.id} chart={chart} index={visibleIndex++} />);
  }
  return <>{panels}</>;
}