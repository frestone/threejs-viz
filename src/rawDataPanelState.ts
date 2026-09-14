// Task 7：单通道 RawData 面板的纯逻辑模型（无 DOM / React / 渲染库依赖，函数无副作用）。
//
// 三块能力：
//  1. panelReducer：单选通道的状态机（definitions/select/frame/decode*/query/nav/viewport）。
//     - stale:true 的解码结果一律忽略；identity(channel+seq) 不匹配的结果也忽略。
//     - 切换通道先取消旧通道（清进行中态与文本）；缺 schema/不可用通道不可选。
//     - decodeFailed 清旧文本但保留选择，下一条有效帧恢复。
//  2. 搜索：Unicode 安全（跨代理对按整字符命中），高亮边界用 JS(UTF-16) 字符串索引。
//  3. visibleRange：虚拟行窗口纯函数，边界夹紧，超大消息只算窗口不遍历全部行。
import type { RawDataDefs } from "./types";
import type { RawDataDecodeResult } from "./engine/rawDataDecoder";

export type PanelStatus = "idle" | "decoding" | "ready" | "error" | "unavailable";

// 单个搜索命中：行号 + 行内 JS 字符起止索引（UTF-16 code unit index，供 UI slice）。
export interface SearchMatch {
  row: number;
  start: number; // 行内 JS 字符串索引（含）
  end: number; // 行内 JS 字符串索引（不含）
}

export interface Viewport {
  scrollTop: number;
  viewportHeight: number;
  rowHeight: number;
}

export const RAW_DATA_PAGE_CHARS = 1000;

export interface PanelState {
  // 可选通道 id 列表（available=true 且能定位到对应 schema 的通道）。
  selectableChannels: string[];
  selected: string | null;
  status: PanelStatus;
  lines: string[];
  text: string;
  error?: string;
  // 当前选择通道正等待解码的目标 seq（identity 校验用）；无则 null。
  pendingSeq: number | null;
  // 已成功呈现的 seq（identity 校验用）。
  currentSeq: number | null;
  query: string;
  matches: SearchMatch[];
  activeMatch: number; // -1 表示无有效命中
  scrollRow: number; // 目标滚动行（activeMatch 指向的行）
  viewport: Viewport;
  currentPage: number;
  pageCount: number;
}

export const initialPanelState: PanelState = {
  selectableChannels: [],
  selected: null,
  status: "idle",
  lines: [],
  text: "",
  error: undefined,
  pendingSeq: null,
  currentSeq: null,
  query: "",
  matches: [],
  activeMatch: -1,
  scrollRow: 0,
  viewport: { scrollTop: 0, viewportHeight: 0, rowHeight: 0 },
  currentPage: 0,
  pageCount: 1,
};

export type PanelAction =
  | { type: "definitions"; defs: RawDataDefs }
  | { type: "select"; channel: string }
  | { type: "frame"; channel: string; seq: number }
  | { type: "decodeStarted"; channel: string; seq: number }
  | { type: "decodeSucceeded"; result: RawDataDecodeResult }
  | { type: "decodeFailed"; channel: string; seq: number; error: string }
  | { type: "cancelDecode" }
  | { type: "query"; query: string }
  | { type: "nextMatch" }
  | { type: "previousMatch" }
  | { type: "setPage"; page: number }
  | { type: "setViewport"; scrollTop: number; viewportHeight: number; rowHeight: number }
  // 取消订阅 / 换源统一入口：回到 initialPanelState 但保留 selectableChannels（定义仍有效）。
  | { type: "reset" };

// 计算可选通道：available=true 且能在 schemas 中定位到 schemaId 的通道。
function computeSelectable(defs: RawDataDefs): string[] {
  const schemaIds = new Set(defs.schemas.map((s) => s.id));
  return defs.rawData
    .filter((c) => c.available && c.schemaId !== undefined && schemaIds.has(c.schemaId))
    .map((c) => c.id);
}

// 清空当前呈现内容（保留选择/查询相关字段由调用方决定）。
function clearedContent(): Pick<PanelState, "lines" | "text" | "matches" | "activeMatch" | "currentPage" | "pageCount"> {
  return { lines: [], text: "", matches: [], activeMatch: -1, currentPage: 0, pageCount: 1 };
}

export function rawDataPageCount(text: string): number {
  return Math.max(1, Math.ceil(Array.from(text).length / RAW_DATA_PAGE_CHARS));
}

export function rawDataPageText(text: string, page: number): string {
  const chars = Array.from(text);
  const count = Math.max(1, Math.ceil(chars.length / RAW_DATA_PAGE_CHARS));
  const safePage = Math.min(Math.max(0, page), count - 1);
  return chars.slice(safePage * RAW_DATA_PAGE_CHARS, (safePage + 1) * RAW_DATA_PAGE_CHARS).join("");
}

function pageForMatch(lines: string[], match: SearchMatch): number {
  let charsBeforeMatch = 0;
  for (let row = 0; row < match.row; row++) {
    charsBeforeMatch += Array.from(lines[row]).length + 1;
  }
  charsBeforeMatch += Array.from(lines[match.row].slice(0, match.start)).length;
  return Math.floor(charsBeforeMatch / RAW_DATA_PAGE_CHARS);
}

// --------------------------- 搜索核心 -------------------------------------
// Unicode 安全匹配：按整字符（Array.from 切码点）判定命中，避免代理对中间误命中；
// 但返回的 start/end 是 JS(UTF-16) 字符串索引（indexOf 语义），供 UI slice 直接使用。
export function computeMatches(lines: string[], query: string): SearchMatch[] {
  if (query.length === 0) return [];
  const out: SearchMatch[] = [];
  // 查询按码点长度（整字符数），用于校验命中位置未落在代理对中间。
  for (let row = 0; row < lines.length; row++) {
    const line = lines[row];
    let from = 0;
    for (;;) {
      const idx = line.indexOf(query, from);
      if (idx < 0) break;
      // 代理对完整性校验：命中起点若落在低代理项(尾)上，说明切进了某字符中间，跳过。
      const code = line.charCodeAt(idx);
      const isLowSurrogate = code >= 0xdc00 && code <= 0xdfff;
      if (!isLowSurrogate) {
        out.push({ row, start: idx, end: idx + query.length });
      }
      // 前进至少一个 code unit，避免死循环。
      from = idx + Math.max(1, query.length);
    }
  }
  return out;
}

// 命中对应的目标滚动行。
function matchScrollRow(matches: SearchMatch[], activeMatch: number, fallback: number): number {
  if (activeMatch >= 0 && activeMatch < matches.length) return matches[activeMatch].row;
  return fallback;
}

// 依据当前 lines + query 重算 matches/activeMatch/scrollRow，返回补丁片段。
function recomputeSearch(
  lines: string[],
  query: string,
  scrollRow: number,
): Pick<PanelState, "matches" | "activeMatch" | "scrollRow"> {
  const matches = computeMatches(lines, query);
  const activeMatch = matches.length > 0 ? 0 : -1;
  return { matches, activeMatch, scrollRow: matchScrollRow(matches, activeMatch, scrollRow) };
}

// --------------------------- Reducer --------------------------------------
export function panelReducer(state: PanelState, action: PanelAction): PanelState {
  switch (action.type) {
    case "definitions": {
      const selectableChannels = computeSelectable(action.defs);
      // 选中通道若在新定义中消失/不可选 -> 清空选择与文本。
      if (state.selected !== null && !selectableChannels.includes(state.selected)) {
        return {
          ...state,
          selectableChannels,
          selected: null,
          status: "idle",
          pendingSeq: null,
          currentSeq: null,
          error: undefined,
          ...clearedContent(),
        };
      }
      return { ...state, selectableChannels };
    }

    case "select": {
      // 缺 schema/不可用/未知通道不可选：拒绝，状态不变。
      if (!state.selectableChannels.includes(action.channel)) {
        return state;
      }
      if (action.channel === state.selected) return state;
      // 切换到新通道：先取消旧通道（清进行中态与文本）。
      return {
        ...state,
        selected: action.channel,
        status: "idle",
        pendingSeq: null,
        currentSeq: null,
        error: undefined,
        ...clearedContent(),
      };
    }

    case "frame": {
      // 仅当前选择通道的新帧才进入解码中；记录目标 seq。
      if (action.channel !== state.selected) return state;
      return { ...state, pendingSeq: action.seq, status: "decoding" };
    }

    case "decodeStarted": {
      if (action.channel !== state.selected) return state;
      if (state.pendingSeq !== null && action.seq !== state.pendingSeq) return state;
      return { ...state, status: "decoding" };
    }

    case "decodeSucceeded": {
      const r = action.result;
      // stale 一律忽略。
      if (r.stale === true) return state;
      // identity：channel 必须匹配当前选择。
      if (r.channel !== state.selected) return state;
      // identity：seq 必须匹配当前 pendingSeq；pending 已完成时也不得回退到旧 seq。
      if (state.pendingSeq !== null && r.seq !== state.pendingSeq) return state;
      if (state.pendingSeq === null && state.currentSeq !== null && r.seq < state.currentSeq) return state;
      const patch = recomputeSearch(r.lines, state.query, 0);
      const currentPage = patch.activeMatch >= 0
        ? pageForMatch(r.lines, patch.matches[patch.activeMatch])
        : 0;
      return {
        ...state,
        lines: r.lines,
        text: r.text,
        status: "ready",
        error: undefined,
        currentSeq: r.seq,
        pendingSeq: null,
        currentPage,
        pageCount: rawDataPageCount(r.text),
        ...patch,
      };
    }

    case "cancelDecode":
      if (state.status !== "decoding") return state;
      return {
      ...state,
        status: state.currentSeq === null ? "idle" : "ready",
        pendingSeq: null,
      };

    case "decodeFailed": {
      if (action.channel !== state.selected) return state;
      if (state.pendingSeq !== null && action.seq !== state.pendingSeq) return state;
      // 清空旧文本但保留选择；下一条有效帧恢复。
      return {
        ...state,
        status: "error",
        error: action.error,
        currentSeq: null,
        pendingSeq: null,
        ...clearedContent(),
      };
    }

    case "query": {
      const patch = recomputeSearch(state.lines, action.query, state.scrollRow);
      const currentPage = patch.activeMatch >= 0
        ? pageForMatch(state.lines, patch.matches[patch.activeMatch])
        : state.currentPage;
      return { ...state, query: action.query, currentPage, ...patch };
    }

    case "nextMatch": {
      if (state.matches.length === 0) return { ...state, activeMatch: -1 };
      const activeMatch = (Math.max(0, state.activeMatch) + 1) % state.matches.length;
      const match = state.matches[activeMatch];
      return {
        ...state,
        activeMatch,
        currentPage: pageForMatch(state.lines, match),
        scrollRow: match.row,
      };
    }

    case "previousMatch": {
      if (state.matches.length === 0) return { ...state, activeMatch: -1 };
      const cur = state.activeMatch < 0 ? 0 : state.activeMatch;
      const activeMatch = (cur - 1 + state.matches.length) % state.matches.length;
      const match = state.matches[activeMatch];
      return {
        ...state,
        activeMatch,
        currentPage: pageForMatch(state.lines, match),
        scrollRow: match.row,
      };
    }

    case "setPage": {
      const currentPage = Math.min(Math.max(0, action.page), state.pageCount - 1);
      return currentPage === state.currentPage
        ? state
        : { ...state, currentPage, scrollRow: 0, viewport: { ...state.viewport, scrollTop: 0 } };
    }

    case "setViewport": {
      return {
        ...state,
        viewport: {
          scrollTop: action.scrollTop,
          viewportHeight: action.viewportHeight,
          rowHeight: action.rowHeight,
        },
      };
    }

    case "reset": {
      // 取消订阅/换源：回到 initialPanelState 但保留 selectableChannels（定义仍有效）。
      // 清 selected/status/lines/text/error/pendingSeq/currentSeq/query/matches/activeMatch/scrollRow/viewport。
      return { ...initialPanelState, selectableChannels: state.selectableChannels };
    }

    default: {
      // 穷尽性保护：未知 action 原样返回。
      return state;
    }
  }
}

// --------------------------- 虚拟行 ---------------------------------------
// 纯函数：给定滚动位置/视口高度/行高/总行数/overscan，返回可见窗口 [start, end)。
// 边界夹紧：start>=0，end<=lineCount，scrollTop 负数/过大均不越界。
// 超大消息：只做 O(1) 算术，不遍历行，故 end-start 恒约等于 可见行数 + 2*overscan。
export function visibleRange(
  scrollTop: number,
  height: number,
  rowHeight: number,
  lineCount: number,
  overscan: number,
): { start: number; end: number } {
  if (lineCount <= 0 || rowHeight <= 0) return { start: 0, end: 0 };
  const safeScroll = Math.max(0, scrollTop);
  const over = Math.max(0, overscan | 0);
  const firstVisible = Math.floor(safeScroll / rowHeight);
  const visibleRows = Math.max(1, Math.ceil(height / rowHeight));
  let start = firstVisible - over;
  let end = firstVisible + visibleRows + over;
  if (start < 0) start = 0;
  if (end > lineCount) end = lineCount;
  if (start > lineCount) start = lineCount;
  if (start > end) start = end;
  return { start, end };
}