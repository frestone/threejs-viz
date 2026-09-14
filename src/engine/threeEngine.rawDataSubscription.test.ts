import { describe, expect, it, vi } from "vitest";
import { createExclusiveRawDataSubscription } from "./threeEngine";

describe("Raw Data 单选 transport 订阅", () => {
  it("首次选择只启用目标通道", () => {
    const send = vi.fn();
    const subscription = createExclusiveRawDataSubscription(send);

    subscription.update("chA", true);

    expect(send.mock.calls).toEqual([["chA", true]]);
  });

  it("切换通道时先取消旧通道再启用新通道", () => {
    const send = vi.fn();
    const subscription = createExclusiveRawDataSubscription(send);

    subscription.update("chA", true);
    subscription.update("chB", true);

    expect(send.mock.calls).toEqual([
      ["chA", true],
      ["chA", false],
      ["chB", true],
    ]);
  });

  it("关闭当前通道时只取消当前订阅", () => {
    const send = vi.fn();
    const subscription = createExclusiveRawDataSubscription(send);

    subscription.update("chA", true);
    subscription.update("chA", false);
    subscription.update("chA", false);

    expect(send.mock.calls).toEqual([
      ["chA", true],
      ["chA", false],
    ]);
  });
});