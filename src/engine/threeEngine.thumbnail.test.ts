import { afterEach, describe, expect, it } from "vitest";
import { createThumbnailStore } from "./threeEngine";

const stores: Array<ReturnType<typeof createThumbnailStore>> = [];

afterEach(() => {
  stores.splice(0).forEach((store) => store.clear());
});

describe("缩略图按时间查询", () => {
  it("返回最近缩略图的 URL、时间和源图像序号", () => {
    const store = createThumbnailStore(() => 7);
    stores.push(store);
    store.handle({
      channel: "camera_front",
      tSec: 10,
      gen: 7,
      kind: "thumbnail",
      seq: 101,
      payload: new Uint8Array([1]),
    });
    store.handle({
      channel: "camera_front",
      tSec: 12,
      gen: 7,
      kind: "thumbnail",
      seq: 202,
      payload: new Uint8Array([2]),
    });

    expect(store.findNearestEntry("camera_front", 11.6)).toMatchObject({ tSec: 12, seq: 202 });
  });
});