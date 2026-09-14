import { describe, it, expect } from "vitest";
import { createBigDataStore } from "./threeEngine";
import { shouldReleaseThumbnail } from "./cameraPreviewTransition";

describe("currentBigData 大数据持有者", () => {
  it("命中当前代次则替换当前帧", () => {
    const store = createBigDataStore(() => 5); // 当前 gen=5
    store.handle({ channel: "cam", tSec: 1, gen: 5, kind: "image", seq: 1, payload: new Uint8Array([1]) });
    const cur = store.get("cam");
    expect(cur?.tSec).toBe(1);
    expect(Array.from(cur!.payload)).toEqual([1]);
  });

  it("过期代次帧被丢弃", () => {
    const store = createBigDataStore(() => 5);
    store.handle({ channel: "cam", tSec: 1, gen: 3, kind: "image", seq: 9, payload: new Uint8Array([9]) });
    expect(store.get("cam")).toBeUndefined();
  });

  it("同 channel 新帧替换旧帧（不累积）", () => {
    const store = createBigDataStore(() => 5);
    store.handle({ channel: "cam", tSec: 1, gen: 5, kind: "image", seq: 1, payload: new Uint8Array([1]) });
    store.handle({ channel: "cam", tSec: 2, gen: 5, kind: "image", seq: 2, payload: new Uint8Array([2]) });
    expect(store.get("cam")?.tSec).toBe(2);
    expect(store.size()).toBe(1);
  });
  it("各 raw 通道分别保留自己的最新 protobuf 帧", () => {
    const store = createBigDataStore(() => 5);
    store.handle({ channel: "rawA", tSec: 1, gen: 5, kind: "raw", seq: 1, payload: new Uint8Array([0xa1]) });
    store.handle({ channel: "rawB", tSec: 2, gen: 5, kind: "raw", seq: 2, payload: new Uint8Array([0xb1]) });
    store.handle({ channel: "rawA", tSec: 3, gen: 5, kind: "raw", seq: 3, payload: new Uint8Array([0xa2]) });

    expect(Array.from(store.get("rawA")!.payload)).toEqual([0xa2]);
    expect(Array.from(store.get("rawB")!.payload)).toEqual([0xb1]);
    expect(store.size()).toBe(2);
  });
});

describe("拖动结束后的高清切换屏障", () => {
  it("仅当高清序号命中保留缩略图时才撤下缩略图", () => {
    expect(shouldReleaseThumbnail(101, 100)).toBe(false);
    expect(shouldReleaseThumbnail(101, 101)).toBe(true);
  });
});