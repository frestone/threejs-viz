import { describe, expect, it } from "vitest";
import { resolvePlaybackTime, shouldReportPlayhead } from "./threeEngine";

describe("未缓存 seek 回填期间的播放位置", () => {
  it("普通帧尚未覆盖 seek 目标时保持目标位置", () => {
    expect(resolvePlaybackTime(20, 5, 20)).toBe(20);
  });

  it("没有等待 seek 回填时按缓存末尾封顶", () => {
    expect(resolvePlaybackTime(20, 5, null)).toBe(5);
  });

  it("缓存覆盖 seek 目标后恢复按缓存末尾封顶", () => {
    expect(resolvePlaybackTime(21, 20, null)).toBe(20);
  });

  it("播放位置未越过缓存时保持不变", () => {
    expect(resolvePlaybackTime(4, 5, null)).toBe(4);
  });
});

describe("Raw Data playhead 上报节流", () => {
  it("正常播放超过 80ms 或位置跳变超过 0.05s 时上报", () => {
    expect(shouldReportPlayhead(181, 100, 1, 1, false)).toBe(true);
    expect(shouldReportPlayhead(150, 100, 1.06, 1, false)).toBe(true);
    expect(shouldReportPlayhead(150, 100, 1.04, 1, false)).toBe(false);
  });

  it("拖动期间仅按 120ms 节流，不因位置跳变高频上报", () => {
    expect(shouldReportPlayhead(219, 100, 20, 1, true)).toBe(false);
    expect(shouldReportPlayhead(220, 100, 20, 1, true)).toBe(true);
  });

  it("强制上报不受节流限制", () => {
    expect(shouldReportPlayhead(100, 100, 1, 1, true, true)).toBe(true);
  });
});