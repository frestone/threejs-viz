// Task 7：单通道状态、搜索与虚拟行模型的测试（纯逻辑，无 DOM/React）。
// 覆盖 plan Step 1-3：reducer 选择/切换/清空/错误保留/恢复；Unicode 搜索
// 跨代理对 + 空查询 + next/previous 环绕；visibleRange 边界夹紧 + 超大消息只出窗口范围。
import { describe, it, expect } from "vitest";
import {
  RAW_DATA_PAGE_CHARS,
  initialPanelState,
  panelReducer,
  rawDataPageCount,
  rawDataPageText,
  visibleRange,
  type PanelState,
  type PanelAction,
} from "./rawDataPanelState";
import type { RawDataDefs } from "./types";
import type { RawDataDecodeResult } from "./engine/rawDataDecoder";

// ---- 测试夹具 ----
const defs = (channels: RawDataDefs["rawData"], schemas?: RawDataDefs["schemas"]): RawDataDefs => ({
  rawData: channels,
  schemas: schemas ?? [{ id: "s1", encoding: "protobuf", dataBase64: "" }],
});

const chA: RawDataDefs["rawData"][number] = {
  id: "A", topic: "/a", label: "通道A", available: true, messageType: "pkg.A", schemaId: "s1",
};
const chB: RawDataDefs["rawData"][number] = {
  id: "B", topic: "/b", label: "通道B", available: true, messageType: "pkg.B", schemaId: "s1",
};
const chUnavailable: RawDataDefs["rawData"][number] = {
  id: "U", topic: "/u", label: "不可用", available: false, unavailableReason: "无 schema",
};

const result = (over: Partial<RawDataDecodeResult>): RawDataDecodeResult => ({
  generation: 0, channel: "A", seq: 1, text: "", lines: [], ...over,
});

// ---------------------------------------------------------------------------
describe("panelReducer：definitions / select / 切换 / 清空", () => {
  it("definitions 更新可选通道集合", () => {
    const s = panelReducer(initialPanelState, { type: "definitions", defs: defs([chA, chB, chUnavailable]) });
    expect(s.selectableChannels).toEqual(["A", "B"]);
    expect(s.selected).toBeNull();
  });

  it("select 选中可用通道 -> decoding 状态", () => {
    let s = panelReducer(initialPanelState, { type: "definitions", defs: defs([chA]) });
    s = panelReducer(s, { type: "select", channel: "A" });
    expect(s.selected).toBe("A");
  });

  it("select 不可用通道被拒绝（状态不变）", () => {
    let s = panelReducer(initialPanelState, { type: "definitions", defs: defs([chA, chUnavailable]) });
    const before = s;
    s = panelReducer(s, { type: "select", channel: "U" });
    expect(s.selected).toBeNull();
    expect(s.selected).toBe(before.selected);
  });

  it("select 未知通道被拒绝", () => {
    let s = panelReducer(initialPanelState, { type: "definitions", defs: defs([chA]) });
    s = panelReducer(s, { type: "select", channel: "ZZZ" });
    expect(s.selected).toBeNull();
  });

  it("切换 A->B 先取消 A：清 A 的进行中态与文本", () => {
    let s = panelReducer(initialPanelState, { type: "definitions", defs: defs([chA, chB]) });
    s = panelReducer(s, { type: "select", channel: "A" });
    s = panelReducer(s, { type: "frame", channel: "A", seq: 1 });
    s = panelReducer(s, { type: "decodeSucceeded", result: result({ channel: "A", seq: 1, text: "hello", lines: ["hello"] }) });
    expect(s.lines).toEqual(["hello"]);
    s = panelReducer(s, { type: "select", channel: "B" });
    expect(s.selected).toBe("B");
    expect(s.lines).toEqual([]);
    expect(s.status).not.toBe("ready");
  });

  it("definitions 后选中通道消失 -> 清空选择与文本", () => {
    let s = panelReducer(initialPanelState, { type: "definitions", defs: defs([chA]) });
    s = panelReducer(s, { type: "select", channel: "A" });
    s = panelReducer(s, { type: "decodeSucceeded", result: result({ channel: "A", seq: 1, text: "x", lines: ["x"] }) });
    s = panelReducer(s, { type: "definitions", defs: defs([chB]) });
    expect(s.selected).toBeNull();
    expect(s.lines).toEqual([]);
  });

  it("definitions 后选中通道变为不可用 -> 清空选择", () => {
    let s = panelReducer(initialPanelState, { type: "definitions", defs: defs([chA]) });
    s = panelReducer(s, { type: "select", channel: "A" });
    const chAgone: RawDataDefs["rawData"][number] = { id: "A", topic: "/a", label: "通道A", available: false, unavailableReason: "掉线" };
    s = panelReducer(s, { type: "definitions", defs: defs([chAgone]) });
   expect(s.selected).toBeNull();
    expect(s.lines).toEqual([]);
  });
});

describe("panelReducer：解码生命周期 / stale 忽略 / 错误保留恢复", () => {
  const base = (): PanelState => {
    let s = panelReducer(initialPanelState, { type: "definitions", defs: defs([chA]) });
    s = panelReducer(s, { type: "select", channel: "A" });
    return s;
  };

  it("frame + decodeStarted -> decoding", () => {
    let s = base();
    s = panelReducer(s, { type: "frame", channel: "A", seq: 5 });
    s = panelReducer(s, { type: "decodeStarted", channel: "A", seq: 5 });
    expect(s.status).toBe("decoding");
  });

  it("decodeSucceeded 非 stale 且 identity 匹配 -> 更新 lines/text 清 error", () => {
    let s = base();
    s = panelReducer(s, { type: "frame", channel: "A", seq: 2 });
    s = panelReducer(s, { type: "decodeSucceeded", result: result({ channel: "A", seq: 2, text: "l1\nl2", lines: ["l1", "l2"] }) });
    expect(s.lines).toEqual(["l1", "l2"]);
    expect(s.status).toBe("ready");
    expect(s.error).toBeUndefined();
  });

  it("stale:true 结果一律忽略（不更新任何可见状态）", () => {
    let s = base();
    s = panelReducer(s, { type: "frame", channel: "A", seq: 3 });
    s = panelReducer(s, { type: "decodeSucceeded", result: result({ channel: "A", seq: 3, text: "good", lines: ["good"] }) });
    const snapshot = s;
    s = panelReducer(s, { type: "decodeSucceeded", result: result({ channel: "A", seq: 2, text: "STALE", lines: ["STALE"], stale: true }) });
    expect(s.lines).toEqual(["good"]);
    expect(s).toEqual(snapshot);
  });

  it("identity 不匹配（非当前 seq）的成功结果被忽略", () => {
    let s = base();
    s = panelReducer(s, { type: "frame", channel: "A", seq: 10 });
    // 到达一个旧 seq 的结果（非 stale 标记，但 identity 不匹配当前 seq）
    s = panelReducer(s, { type: "decodeSucceeded", result: result({ channel: "A", seq: 4, text: "old", lines: ["old"] }) });
    expect(s.lines).toEqual([]);
  });

  it("较新结果完成后到达的旧 seq 结果不会覆盖文本", () => {
    let s = base();
    s = panelReducer(s, { type: "frame", channel: "A", seq: 2 });
    s = panelReducer(s, { type: "decodeSucceeded", result: result({ channel: "A", seq: 2, text: "new", lines: ["new"] }) });
    s = panelReducer(s, { type: "decodeSucceeded", result: result({ channel: "A", seq: 1, text: "old", lines: ["old"] }) });
    expect(s.text).toBe("new");
    expect(s.currentSeq).toBe(2);
  });

  it("channel 不匹配的成功结果被忽略", () => {
    let s = base();
    s = panelReducer(s, { type: "frame", channel: "A", seq: 1 });
    s = panelReducer(s, { type: "decodeSucceeded", result: result({ channel: "B", seq: 1, text: "wrong", lines: ["wrong"] }) });
    expect(s.lines).toEqual([]);
  });

  it("decodeFailed 清空旧文本但保留选择；下一条有效帧恢复文本", () => {
    let s = base();
    s = panelReducer(s, { type: "frame", channel: "A", seq: 1 });
    s = panelReducer(s, { type: "decodeSucceeded", result: result({ channel: "A", seq: 1, text: "ok", lines: ["ok"] }) });
    expect(s.lines).toEqual(["ok"]);
    s = panelReducer(s, { type: "decodeFailed", channel: "A", seq: 2, error: "boom" });
    expect(s.selected).toBe("A");
    expect(s.lines).toEqual([]);
    expect(s.status).toBe("error");
    expect(s.error).toBe("boom");
    // 下一条成功帧恢复
    s = panelReducer(s, { type: "frame", channel: "A", seq: 3 });
    s = panelReducer(s, { type: "decodeSucceeded", result: result({ channel: "A", seq: 3, text: "back", lines: ["back"] }) });
    expect(s.lines).toEqual(["back"]);
    expect(s.status).toBe("ready");
    expect(s.error).toBeUndefined();
  });
});

describe("搜索：Unicode 安全 / 空查询 / next-previous 环绕", () => {
  const withLines = (lines: string[]): PanelState => {
    let s = panelReducer(initialPanelState, { type: "definitions", defs: defs([chA]) });
    s = panelReducer(s, { type: "select", channel: "A" });
    s = panelReducer(s, { type: "frame", channel: "A", seq: 1 });
    return panelReducer(s, { type: "decodeSucceeded", result: result({ channel: "A", seq: 1, text: lines.join("\n"), lines }) });
  };

  it("空查询 => 零结果，activeMatch=-1", () => {
    let s = withLines(["abc", "def"]);
    s = panelReducer(s, { type: "query", query: "" });
    expect(s.matches).toEqual([]);
    expect(s.activeMatch).toBe(-1);
  });

  it("基本匹配：命中行号 + JS 字符起止索引", () => {
    let s = withLines(["foo bar", "bar foo bar"]);
    s = panelReducer(s, { type: "query", query: "bar" });
    expect(s.matches.length).toBe(3);
    expect(s.matches[0]).toMatchObject({ row: 0, start: 4, end: 7 });
    expect(s.matches[1]).toMatchObject({ row: 1, start: 0, end: 3 });
    expect(s.matches[2]).toMatchObject({ row: 1, start: 8, end: 11 });
    expect(s.activeMatch).toBe(0);
  });

  it("跨代理对：emoji 后的目标整字符命中，边界用 JS(UTF-16) 索引", () => {
    // "😀X" : 😀 占 2 个 UTF-16 code unit，X 在 JS 索引 2
    const line = "😀X😀";
    let s = withLines([line]);
    s = panelReducer(s, { type: "query", query: "X" });
    expect(s.matches.length).toBe(1);
    // JS 索引：indexOf("X") === 2
    expect(s.matches[0]).toMatchObject({ row: 0, start: 2, end: 3 });
    // 用返回的 JS 索引 slice 应还原为 "X"
    expect(line.slice(s.matches[0].start, s.matches[0].end)).toBe("X");
  });

  it("按整字符匹配 emoji 本身（跨代理对整字符命中）", () => {
    const line = "a😀b";
    let s = withLines([line]);
    s = panelReducer(s, { type: "query", query: "😀" });
    expect(s.matches.length).toBe(1);
    expect(s.matches[0]).toMatchObject({ row: 0, start: 1, end: 3 });
    expect(line.slice(s.matches[0].start, s.matches[0].end)).toBe("😀");
  });

  it("next/previous 环绕并更新 scrollRow", () => {
    let s = withLines(["x", "hit", "y", "z", "hit"]);
    s = panelReducer(s, { type: "query", query: "hit" });
    expect(s.matches.length).toBe(2);
    expect(s.activeMatch).toBe(0);
    expect(s.scrollRow).toBe(1);
    // next -> 第二个
    s = panelReducer(s, { type: "nextMatch" });
    expect(s.activeMatch).toBe(1);
    expect(s.scrollRow).toBe(4);
    // next 环绕回第一个
    s = panelReducer(s, { type: "nextMatch" });
    expect(s.activeMatch).toBe(0);
    expect(s.scrollRow).toBe(1);
    // previous 环绕到最后一个
    s = panelReducer(s, { type: "previousMatch" });
    expect(s.activeMatch).toBe(1);
    expect(s.scrollRow).toBe(4);
  });

  it("无匹配时 next/previous 不越界", () => {
    let s = withLines(["a", "b"]);
    s = panelReducer(s, { type: "query", query: "zzz" });
    expect(s.matches).toEqual([]);
    s = panelReducer(s, { type: "nextMatch" });
    expect(s.activeMatch).toBe(-1);
    s = panelReducer(s, { type: "previousMatch" });
    expect(s.activeMatch).toBe(-1);
  });

  it("新解码结果到达后重算 matches（沿用当前 query）", () => {
    let s = withLines(["nope"]);
    s = panelReducer(s, { type: "query", query: "hit" });
    expect(s.matches).toEqual([]);
    s = panelReducer(s, { type: "frame", channel: "A", seq: 2 });
    s = panelReducer(s, { type: "decodeSucceeded", result: result({ channel: "A", seq: 2, text: "hit here", lines: ["hit here"] }) });
    expect(s.matches.length).toBe(1);
    expect(s.matches[0]).toMatchObject({ row: 0, start: 0, end: 3 });
  });

  it("非空搜索下新解码结果自动切到首个命中所在页", () => {
    let s = withLines(["nope"]);
    s = panelReducer(s, { type: "query", query: "target" });

    const text = "x".repeat(RAW_DATA_PAGE_CHARS + 10) + "target";
    s = panelReducer(s, { type: "frame", channel: "A", seq: 2 });
    s = panelReducer(s, {
      type: "decodeSucceeded",
      result: result({ channel: "A", seq: 2, text, lines: [text] }),
    });

    expect(s.matches).toHaveLength(1);
    expect(s.activeMatch).toBe(0);
    expect(s.currentPage).toBe(1);
  });
});

describe("1000 字符分页", () => {
  it("按 Unicode code point 分页且页间无遗漏", () => {
    const text = "中".repeat(999) + "😀" + "尾";
    expect(Array.from(text)).toHaveLength(1001);
    expect(rawDataPageCount(text)).toBe(2);
    const first = rawDataPageText(text, 0);
    const second = rawDataPageText(text, 1);
    expect(Array.from(first)).toHaveLength(RAW_DATA_PAGE_CHARS);
    expect(first.endsWith("😀")).toBe(true);
    expect(second).toBe("尾");
    expect(first + second).toBe(text);
  });

  it("空文本仍为一页，越界页码夹紧", () => {
    expect(rawDataPageCount("")).toBe(1);
    expect(rawDataPageText("abc", -1)).toBe("abc");
    expect(rawDataPageText("x".repeat(1001), 99)).toBe("x");
  });

  it("解码结果维护页数，setPage 夹紧并重置滚动", () => {
    let s = panelReducer(initialPanelState, { type: "definitions", defs: defs([chA]) });
    s = panelReducer(s, { type: "select", channel: "A" });
    s = panelReducer(s, { type: "frame", channel: "A", seq: 1 });
    const text = "x".repeat(2001);
    s = panelReducer(s, { type: "decodeSucceeded", result: result({ text, lines: [text] }) });
    expect(s.pageCount).toBe(3);
    expect(s.currentPage).toBe(0);
    s = panelReducer(s, { type: "setViewport", scrollTop: 300, viewportHeight: 100, rowHeight: 20 });
    s = panelReducer(s, { type: "setPage", page: 99 });
    expect(s.currentPage).toBe(2);
    expect(s.viewport.scrollTop).toBe(0);
    s = panelReducer(s, { type: "setPage", page: -1 });
    expect(s.currentPage).toBe(0);
  });

  it("搜索导航切换到全文命中所在页", () => {
    const text = "x".repeat(1010) + "target";
    let s = panelReducer(initialPanelState, { type: "definitions", defs: defs([chA]) });
    s = panelReducer(s, { type: "select", channel: "A" });
    s = panelReducer(s, { type: "frame", channel: "A", seq: 1 });
    s = panelReducer(s, { type: "decodeSucceeded", result: result({ text, lines: [text] }) });
    s = panelReducer(s, { type: "query", query: "target" });
    expect(s.currentPage).toBe(1);
    expect(s.matches).toHaveLength(1);
  });
});

describe("visibleRange：边界夹紧 / 超大消息只出窗口", () => {
  it("常规窗口", () => {
    const r = visibleRange(100, 200, 20, 1000, 2);
    // start = floor(100/20)-2 = 3, 可见行数 ceil(200/20)=10, end = 5+10+2*2?用实现约束校验
    expect(r.start).toBeGreaterThanOrEqual(0);
    expect(r.end).toBeLessThanOrEqual(1000);
    expect(r.end).toBeGreaterThan(r.start);
  });

  it("scrollTop 负数夹紧到 0", () => {
    const r = visibleRange(-500, 200, 20, 1000, 3);
    expect(r.start).toBe(0);
    expect(r.end).toBeLessThanOrEqual(1000);
  });

  it("scrollTop 过大 -> end 夹紧到 lineCount，start 不越界", () => {
    const r = visibleRange(1e9, 200, 20, 50, 2);
    expect(r.end).toBe(50);
    expect(r.start).toBeGreaterThanOrEqual(0);
    expect(r.start).toBeLessThanOrEqual(50);
  });

  it("lineCount=0 -> 空窗口", () => {
    const r = visibleRange(0, 200, 20, 0, 2);
    expect(r.start).toBe(0);
    expect(r.end).toBe(0);
  });

  it("超大消息(2,000,000 行)只出窗口范围，不遍历全部", () => {
    const rowHeight = 20;
    const height = 400;
    const overscan = 5;
    const lineCount = 2_000_000;
    const r = visibleRange(1000 * rowHeight, height, rowHeight, lineCount, overscan);
    const windowRows = r.end - r.start;
    const visibleRows = Math.ceil(height / rowHeight);
    // 窗口约等于 可见行数 + 2*overscan（含首尾 overscan）
    expect(windowRows).toBeLessThanOrEqual(visibleRows + 2 * overscan + 2);
    expect(windowRows).toBeGreaterThanOrEqual(visibleRows);
    expect(r.end).toBeLessThanOrEqual(lineCount);
  });
});

describe("reducer 纯度：不 mutate 入参", () => {
  it("返回新 state，原 state 不被修改", () => {
    const s0 = panelReducer(initialPanelState, { type: "definitions", defs: defs([chA]) });
    const frozen = JSON.parse(JSON.stringify(s0));
    const s1 = panelReducer(s0, { type: "select", channel: "A" });
    expect(s0).toEqual(frozen);
    expect(s1).not.toBe(s0);
  });
});

describe("setViewport", () => {
  it("更新滚动/视口参数", () => {
    let s = panelReducer(initialPanelState, { type: "definitions", defs: defs([chA]) });
    s = panelReducer(s, { type: "setViewport", scrollTop: 240, viewportHeight: 400, rowHeight: 20 });
    expect(s.viewport.scrollTop).toBe(240);
    expect(s.viewport.viewportHeight).toBe(400);
    expect(s.viewport.rowHeight).toBe(20);
  });
});

describe("reset：保留 selectableChannels，清其余", () => {
  it("回到 initialPanelState 但保留 selectableChannels", () => {
    // 构造一个内容丰满的 state：选中通道、有解码文本、查询命中、视口滚动。
    let s = panelReducer(initialPanelState, { type: "definitions", defs: defs([chA, chB]) });
    s = panelReducer(s, { type: "select", channel: "A" });
    s = panelReducer(s, { type: "frame", channel: "A", seq: 3 });
    s = panelReducer(s, {
      type: "decodeSucceeded",
      result: result({ channel: "A", seq: 3, text: "hit line", lines: ["hit line", "second"] }),
    });
    s = panelReducer(s, { type: "query", query: "hit" });
    s = panelReducer(s, { type: "setViewport", scrollTop: 100, viewportHeight: 400, rowHeight: 20 });
    expect(s.selected).toBe("A");
    expect(s.lines.length).toBeGreaterThan(0);
    expect(s.matches.length).toBeGreaterThan(0);

    const r = panelReducer(s, { type: "reset" });
    // 保留 selectableChannels
    expect(r.selectableChannels).toEqual(["A", "B"]);
    // 其余全部回初始
    expect(r.selected).toBeNull();
    expect(r.status).toBe("idle");
    expect(r.lines).toEqual([]);
    expect(r.text).toBe("");
    expect(r.error).toBeUndefined();
    expect(r.pendingSeq).toBeNull();
    expect(r.currentSeq).toBeNull();
    expect(r.query).toBe("");
    expect(r.matches).toEqual([]);
    expect(r.activeMatch).toBe(-1);
    expect(r.scrollRow).toBe(0);
    expect(r.viewport).toEqual({ scrollTop: 0, viewportHeight: 0, rowHeight: 0 });
  });

  it("空定义 reset：selectableChannels 保持空", () => {
    const r = panelReducer(initialPanelState, { type: "reset" });
    expect(r.selectableChannels).toEqual([]);
    expect(r.selected).toBeNull();
  });
});