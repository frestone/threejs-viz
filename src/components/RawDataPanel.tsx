// Task 9：右侧可停靠 RawData 面板（受控组件，props 驱动，无内部业务逻辑）。
//
// 硬约束体现：
//  - 组件内绝不实现 protobuf 解码：只渲染 PanelState + 触发回调，解码全在引擎/Worker。
//  - 虚拟列表只渲染可见行：用 visibleRange 计算窗口，绝不一次性渲染全部 lines。
//  - 订阅默认关闭：初始不选通道（由 App 保证），选中才 onSelectChannel(channel)。
//  - 单一真相源：所有交互只 dispatch 回调，state 唯一来自 props（由 panelReducer 演进）。
//  - 错误隔离：面板自身渲染异常不冒泡（仅本组件树）。
import { useEffect, useMemo, useRef, useState } from "react";
import type { PanelState, SearchMatch } from "../rawDataPanelState";
import {
  computeMatches,
  RAW_DATA_PAGE_CHARS,
  rawDataPageText,
  visibleRange,
} from "../rawDataPanelState";
import type { RawDataChannelDef } from "../types";

interface Props {
  state: PanelState;
  defs: RawDataChannelDef[];
  onSelectChannel: (channel: string | null) => void;
  onQuery: (query: string) => void;
  onNextMatch: () => void;
  onPrevMatch: () => void;
  onPageChange: (page: number) => void;
  onViewport: (scrollTop: number, viewportHeight: number, rowHeight: number) => void;
  onVisibilityChange?: (visible: boolean) => void;
}

const WIDTH_KEY = "rawpanel.width";
const DEFAULT_WIDTH = 420;
const MIN_WIDTH = 240;
const MAX_WIDTH = 900;
const ROW_HEIGHT = 20; // 单行文本高度（px），虚拟化窗口与总高计算用
const OVERSCAN = 6; // 视口上下额外预渲染行数，减少快速滚动白屏

export function measureRawDataViewport(
  element: Pick<HTMLDivElement, "scrollTop" | "clientHeight">,
  onViewport: Props["onViewport"],
): void {
  onViewport(element.scrollTop, element.clientHeight, ROW_HEIGHT);
}

// 渲染单行：把当前行内的 matches 切成 高亮 / 普通 片段。activeMatch 命中用强高亮。
function renderLine(
  line: string,
  row: number,
  matches: SearchMatch[],
  activeMatchIndex: number,
): React.ReactNode {
  // 收集落在本行的命中（携带其在 matches 数组内的全局序号，用于判定是否 active）。
  const inRow: { m: SearchMatch; gi: number }[] = [];
  for (let gi = 0; gi < matches.length; gi++) {
    if (matches[gi].row === row) inRow.push({ m: matches[gi], gi });
  }
  if (inRow.length === 0) return line;
  const parts: React.ReactNode[] = [];
  let cursor = 0;
  for (let i = 0; i < inRow.length; i++) {
    const { m, gi } = inRow[i];
    if (m.start > cursor) parts.push(line.slice(cursor, m.start));
    const isActive = gi === activeMatchIndex;
    parts.push(
      <mark
        key={`m${gi}`}
        className={isActive ? "rawpanel-hit rawpanel-hit-active" : "rawpanel-hit"}
      >
        {line.slice(m.start, m.end)}
      </mark>,
    );
    cursor = m.end;
  }
  if (cursor < line.length) parts.push(line.slice(cursor));
  return parts;
}

// 浮于右侧的可停靠 RawData 面板：分隔条调宽（sessionStorage 记忆）+ 折叠 + 通道选择 + 搜索 + 虚拟化文本。
export function RawDataPanel({
  state,
  defs,
  onSelectChannel,
  onQuery,
  onNextMatch,
  onPrevMatch,
  onPageChange,
  onViewport,
  onVisibilityChange,
}: Props) {
  const [width, setWidth] = useState<number>(() => {
    try {
      const s = sessionStorage.getItem(WIDTH_KEY);
      if (s) {
        const n = Number(JSON.parse(s));
        if (Number.isFinite(n)) return Math.min(MAX_WIDTH, Math.max(MIN_WIDTH, n));
      }
    } catch {}
    return DEFAULT_WIDTH;
  });
  const [collapsed, setCollapsed] = useState(false);
  const [queryInput, setQueryInput] = useState(state.query);
  const scrollRef = useRef<HTMLDivElement>(null);
  const resizeRef = useRef<{ startX: number; baseW: number } | null>(null);

  // 宽度记忆：写入 sessionStorage（类比 CameraOverlay 的持久化模式）。
  useEffect(() => {
    try {
      sessionStorage.setItem(WIDTH_KEY, JSON.stringify(width));
    } catch {}
  }, [width]);

  // 外部 state.query 变化（如 reset/换通道）时同步回输入框，保持单一真相源。
  useEffect(() => {
    setQueryInput(state.query);
  }, [state.query]);

  // scrollRow 变化（activeMatch 导航）时把目标行滚到视口内。
  useEffect(() => {
    const el = scrollRef.current;
    if (!el) return;
    const targetTop = state.scrollRow * ROW_HEIGHT;
    if (targetTop < el.scrollTop || targetTop > el.scrollTop + el.clientHeight - ROW_HEIGHT) {
      el.scrollTop = targetTop;
    }
  }, [state.scrollRow]);

  const dragResize = (e: React.MouseEvent) => {
    resizeRef.current = { startX: e.clientX, baseW: width };
    const onMove = (ev: MouseEvent) => {
      const r = resizeRef.current;
      if (!r) return;
      // 分隔条在左沿：向左拖(clientX 减小)加宽。
      const next = r.baseW + (r.startX - ev.clientX);
      setWidth(Math.min(MAX_WIDTH, Math.max(MIN_WIDTH, next)));
    };
    const onUp = () => {
      resizeRef.current = null;
      window.removeEventListener("mousemove", onMove);
      window.removeEventListener("mouseup", onUp);
    };
    window.addEventListener("mousemove", onMove);
    window.addEventListener("mouseup", onUp);
  };

  const onScroll = (e: React.UIEvent<HTMLDivElement>) => {
    measureRawDataViewport(e.currentTarget, onViewport);
  };

  // 当前页按 Unicode code point 切分；完整文本与全文搜索结果仍保留在 reducer 中。
  const pageChars = useMemo(() => Array.from(state.text), [state.text]);
  const pageText = useMemo(
    () => rawDataPageText(state.text, state.currentPage),
    [state.text, state.currentPage],
  );
  const pageLines = useMemo(() => pageText.split("\n"), [pageText]);
  const pageMatches = useMemo(() => computeMatches(pageLines, state.query), [pageLines, state.query]);
  const matchesBeforePage = useMemo(() => {
    if (!state.query || state.currentPage === 0) return 0;
    const prefix = pageChars.slice(0, state.currentPage * RAW_DATA_PAGE_CHARS).join("");
    return computeMatches(prefix.split("\n"), state.query).length;
  }, [pageChars, state.currentPage, state.query]);
  const activePageMatch = state.activeMatch - matchesBeforePage;

  // 文本列表仅在 ready 时挂载；首次挂载、换页/换文本及面板尺寸变化时主动测量。
  // 不能只依赖 scroll：初始 viewportHeight=0 会让虚拟列表只渲染 overscan的少量行。
  useEffect(() => {
    const el = scrollRef.current;
    if (!el || state.status !== "ready" || state.lines.length === 0) return;
    const measure = () => measureRawDataViewport(el, onViewport);
    measure();
    if (typeof ResizeObserver === "undefined") return;
    const observer = new ResizeObserver(measure);
    observer.observe(el);
    return () => observer.disconnect();
  }, [state.status, state.text, state.currentPage, width, collapsed, onViewport]);

  // 虚拟化窗口：只渲染当前页的 [start,end) 行。
  const { scrollTop, viewportHeight } = state.viewport;
  const range = useMemo(
    () => visibleRange(scrollTop, viewportHeight, ROW_HEIGHT, pageLines.length, OVERSCAN),
    [scrollTop, viewportHeight, pageLines.length],
  );
  const totalHeight = pageLines.length * ROW_HEIGHT;
  const matchCount = state.matches.length;
  const matchLabel = matchCount > 0 ? `${state.activeMatch + 1} / ${matchCount}` : "0";

  if (collapsed) {
    return (
      <div className="rawpanel-collapsed">
        <button
          type="button"
          onClick={() => {
            setCollapsed(false);
            onVisibilityChange?.(true);
          }}
          title="展开 RawData 面板"
        >
          RawData ◀
        </button>
      </div>
    );
  }

  return (
    <aside className="rawpanel" style={{ width }} data-testid="rawpanel">
      <div className="rawpanel-resizer" onMouseDown={dragResize} title="拖动调宽" />
      <div className="rawpanel-header">
        <span className="rawpanel-title">RawData</span>
        <button
          type="button"
          onClick={() => {
            setCollapsed(true);
            onVisibilityChange?.(false);
          }}
          title="折叠面板"
        >
          ▶
        </button>
      </div>

      <div className="rawpanel-channels">
        {defs.length === 0 && <div className="rawpanel-empty">无 RawData 通道</div>}
        {defs.map((d) => {
          const isSelected = state.selected === d.id;
          return (
            <label
              key={d.id}
              className={`rawpanel-channel${d.available ? "" : " disabled"}${isSelected ? " selected" : ""}`}
              title={d.available ? d.topic : d.unavailableReason ?? "不可用"}
            >
              <input
                type="radio"
                name="rawpanel-channel"
                disabled={!d.available}
                checked={isSelected}
                onChange={() => onSelectChannel(isSelected ? null : d.id)}
                onClick={() => {
                  if (isSelected) onSelectChannel(null); // 再次点当前通道 → 取消订阅
                }}
              />
              <span className="rawpanel-channel-label">{d.label}</span>
              {!d.available && (
                <span className="rawpanel-channel-reason">{d.unavailableReason ?? "不可用"}</span>
              )}
            </label>
          );
        })}
        {state.selected !== null && (
          <button type="button" className="rawpanel-clear" onClick={() => onSelectChannel(null)}>
            取消选择
          </button>
        )}
      </div>

      <div className="rawpanel-search">
        <input
          type="text"
          placeholder="搜索…"
          value={queryInput}
          onChange={(e) => setQueryInput(e.target.value)}
          onKeyDown={(e) => {
            if (e.key === "Enter") onQuery(queryInput);
          }}
          data-testid="rawpanel-search-input"
        />
        <button
          type="button"
          className="rawpanel-search-button"
          onClick={() => onQuery(queryInput)}
          data-testid="rawpanel-search-button"
        >
          搜索
        </button>
        <span className="rawpanel-match-count" data-testid="rawpanel-match-count">
          {matchLabel}
        </span>
        <button type="button" onClick={onPrevMatch} disabled={matchCount === 0} title="上一个">
          ▲
        </button>
        <button type="button" onClick={onNextMatch} disabled={matchCount === 0} title="下一个">
          ▼
        </button>
      </div>

      <div className="rawpanel-pagination" data-testid="rawpanel-pagination">
        <button type="button" onClick={() => onPageChange(state.currentPage - 1)} disabled={state.currentPage === 0}>
          上一页
        </button>
        <span>{state.currentPage + 1} / {state.pageCount}</span>
        <button type="button" onClick={() => onPageChange(state.currentPage + 1)} disabled={state.currentPage >= state.pageCount - 1}>
          下一页
        </button>
      </div>

      <div className="rawpanel-body">
        {state.status === "decoding" && <div className="rawpanel-status">解码中…</div>}
        {state.status === "error" && (
          <div className="rawpanel-status rawpanel-error" data-testid="rawpanel-error">
            {state.error ?? "解码失败"}
          </div>
        )}
        {state.status === "unavailable" && (
          <div className="rawpanel-status">该通道不可解码</div>
        )}
        {state.selected === null && state.status === "idle" && (
          <div className="rawpanel-status">请选择一个通道</div>
        )}
        {state.status === "ready" && state.lines.length === 0 && (
          <div className="rawpanel-status">（空消息）</div>
        )}
        {state.status === "ready" && state.lines.length > 0 && (
          <div
            className="rawpanel-lines"
            ref={scrollRef}
            onScroll={onScroll}
            data-testid="rawpanel-lines"
          >
            <div style={{ height: totalHeight, position: "relative" }}>
              {Array.from({ length: range.end - range.start }, (_, i) => {
                const row = range.start + i;
                return (
                  <div
                    key={row}
                    className="rawpanel-line"
                    style={{
                      position: "absolute",
                      top: row * ROW_HEIGHT,
                      height: ROW_HEIGHT,
                      left: 0,
                      right: 0,
                    }}
                    data-row={row}
                  >
                    {renderLine(pageLines[row], row, pageMatches, activePageMatch)}
                  </div>
                );
              })}
            </div>
          </div>
        )}
      </div>
    </aside>
  );
}