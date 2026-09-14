// Task 9：RawDataPanel 组件测试。
//
// 无 jsdom/testing-library 依赖：用 react-dom/server 的 renderToStaticMarkup 做无 DOM
// 静态渲染断言，覆盖 idle/decoding/ready/error/空态、通道可选/禁用+原因、匹配计数、
// 虚拟化只渲可见行（visibleRange 驱动）、默认不选通道（订阅默认关闭）。
import { describe, it, expect, beforeEach } from "vitest";
import { renderToStaticMarkup } from "react-dom/server";
import { RawDataPanel, measureRawDataViewport } from "./RawDataPanel";
import { initialPanelState, type PanelState } from "../rawDataPanelState";
import type { RawDataChannelDef } from "../types";

// sessionStorage 在 node 环境缺失：注入最小 stub，避免宽度记忆 effect/初值读取报错。
beforeEach(() => {
  const store = new Map<string, string>();
  (globalThis as unknown as { sessionStorage: Storage }).sessionStorage = {
    getItem: (k: string) => store.get(k) ?? null,
    setItem: (k: string, v: string) => void store.set(k, v),
    removeItem: (k: string) => void store.delete(k),
    clear: () => store.clear(),
    key: () => null,
    length: 0,
  } as unknown as Storage;
});

describe("RawDataPanel 视口测量", () => {
  it("首次挂载可用同一测量逻辑提交真实视口高度", () => {
    const calls: number[][] = [];
    measureRawDataViewport(
      { scrollTop: 40, clientHeight: 360 },
      (...args) => calls.push(args),
    );
    expect(calls).toEqual([[40, 360, 20]]);
  });
});

const noop = () => {};
const baseProps = {
  onSelectChannel: noop,
  onQuery: noop,
  onNextMatch: noop,
  onPrevMatch: noop,
  onPageChange: noop,
  onViewport: noop,
};

const defs: RawDataChannelDef[] = [
  { id: "c1", topic: "/a", label: "Channel A", available: true, messageType: "M", schemaId: "1" },
  {
    id: "c2",
    topic: "/b",
    label: "Channel B",
    available: false,
    unavailableReason: "缺少 schema",
  },
];

function render(state: PanelState, channelDefs: RawDataChannelDef[] = defs): string {
  return renderToStaticMarkup(
    <RawDataPanel state={state} defs={channelDefs} {...baseProps} />,
  );
}

describe("RawDataPanel 状态渲染", () => {
  it("默认（idle 且未选通道）提示选择通道，且不订阅任何通道", () => {
    const html = render(initialPanelState);
    expect(html).toContain("请选择一个通道");
    // 无 checked radio → 默认不选（订阅默认关闭）。
    expect(html).not.toContain("checked");
  });

  it("decoding 显示解码中", () => {
    const html = render({ ...initialPanelState, selected: "c1", status: "decoding" });
    expect(html).toContain("解码中");
  });

  it("error 显示错误信息", () => {
    const html = render({
      ...initialPanelState,
      selected: "c1",
      status: "error",
      error: "boom",
    });
    expect(html).toContain("boom");
  });

  it("unavailable 显示不可解码", () => {
    const html = render({ ...initialPanelState, selected: "c1", status: "unavailable" });
    expect(html).toContain("该通道不可解码");
  });

  it("ready 且 lines 为空显示空消息态", () => {
    const html = render({
      ...initialPanelState,
      selected: "c1",
      status: "ready",
      lines: [],
    });
    expect(html).toContain("空消息");
  });
});

describe("RawDataPanel 通道选择", () => {
  it("available 通道可选、unavailable 通道禁用并展示原因", () => {
    const html = render(initialPanelState);
    expect(html).toContain("Channel A");
    expect(html).toContain("Channel B");
    expect(html).toContain("缺少 schema"); // 禁用原因展示
    expect(html).toContain("disabled"); // c2 radio disabled
  });

  it("选中通道时对应 radio checked，并出现取消选择按钮", () => {
    const html = render({ ...initialPanelState, selected: "c1" });
    expect(html).toContain("checked");
    expect(html).toContain("取消选择");
  });
});

describe("RawDataPanel 搜索提交", () => {
  it("渲染显式搜索按钮", () => {
    const html = render(initialPanelState);
    expect(html).toContain('data-testid="rawpanel-search-button"');
    expect(html).toContain(">搜索</button>");
  });
});

describe("RawDataPanel 搜索匹配计数", () => {
  it("无匹配显示 0", () => {
    const html = render({
      ...initialPanelState,
      selected: "c1",
      status: "ready",
      lines: ["x"],
      query: "z",
      matches: [],
      activeMatch: -1,
    });
    expect(html).toContain(">0<"); // match-count span 文本为 "0"
  });

  it("有匹配显示 activeMatch+1 / total", () => {
    const html = render({
      ...initialPanelState,
      selected: "c1",
      status: "ready",
      lines: ["foo foo"],
      query: "foo",
      matches: [
        { row: 0, start: 0, end: 3 },
        { row: 0, start: 4, end: 7 },
      ],
      activeMatch: 1,
    });
    expect(html).toContain("2 / 2");
  });
});

describe("RawDataPanel 虚拟化", () => {
  it("超大 lines 只渲染可见窗口行（visibleRange 驱动），不渲染全部", () => {
    const lines = Array.from({ length: 100 }, (_, i) => `L${i}`);
    const text = lines.join("\n");
    const html = render({
      ...initialPanelState,
      selected: "c1",
      status: "ready",
      text,
      lines,
      // 视口高 100px / 行高 20px ≈ 5 可见行 + overscan，远小于当前页行数。
      viewport: { scrollTop: 0, viewportHeight: 100, rowHeight: 20 },
    });
    // 只渲染前若干行，不应出现末尾行。
    expect(html).toContain(">L0<");
    expect(html).not.toContain(">L99<");
    // 渲染的 rawpanel-line 行数应远小于 10000（粗略上界断言）。
    const rowCount = (html.match(/class="rawpanel-line"/g) ?? []).length;
    expect(rowCount).toBeGreaterThan(0);
    expect(rowCount).toBeLessThan(50);
  });

  it("滚动到中部时渲染中部行、不渲染首尾（窗口随 scrollTop 移动）", () => {
    const lines = Array.from({ length: 100 }, (_, i) => `L${i}`);
    const text = lines.join("\n");
    const html = render({
      ...initialPanelState,
      selected: "c1",
    status: "ready",
      text,
      lines,
      viewport: { scrollTop: 500, viewportHeight: 100, rowHeight: 20 },
    });
    // scrollTop 500 / rowHeight 20 = 第 25 行附近。
    expect(html).toContain(">L25<");
    expect(html).not.toContain(">L0<");
    expect(html).not.toContain(">L99<");
  });
});

describe("RawDataPanel 分页", () => {
  it("只渲染当前 1000 字符页并显示页码", () => {
    const text = "a".repeat(1000) + "SECOND_PAGE";
    const html = render({
      ...initialPanelState,
      selected: "c1",
      status: "ready",
      text,
      lines: [text],
      currentPage: 1,
      pageCount: 2,
      viewport: { scrollTop: 0, viewportHeight: 100, rowHeight: 20 },
    });
    expect(html).toContain("2 / 2");
    expect(html).toContain("SECOND_PAGE");
    expect(html).not.toContain("a".repeat(100));
  });

  it("第一页禁用上一页，末页禁用下一页", () => {
    const common = {
      ...initialPanelState,
      selected: "c1",
      status: "ready" as const,
      text: "x".repeat(501),
      lines: ["x".repeat(501)],
      pageCount: 2,
    };
    const first = render({ ...common, currentPage: 0 });
    expect(first).toMatch(/<button[^>]*disabled=""[^>]*>上一页<\/button>/);
    const last = render({ ...common, currentPage: 1 });
    expect(last).toMatch(/<button[^>]*disabled=""[^>]*>下一页<\/button>/);
  });
});

describe("RawDataPanel 搜索高亮", () => {
  it("命中片段用 mark 包裹，activeMatch 用强高亮 class", () => {
    const text = "hello world";
    const html = render({
      ...initialPanelState,
      selected: "c1",
      status: "ready",
      text,
      lines: [text],
      query: "o",
      matches: [
        { row: 0, start: 4, end: 5 },
        { row: 0, start: 7, end: 8 },
      ],
      activeMatch: 0,
      viewport: { scrollTop: 0, viewportHeight: 100, rowHeight: 20 },
    });
    expect(html).toContain("rawpanel-hit-active"); // 第 0 个命中为 active
    expect(html).toContain("<mark");
  });
});