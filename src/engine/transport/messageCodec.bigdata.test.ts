import { describe, it, expect } from "vitest";
import { parseServerMessage, BIGDATA_MESSAGE_TYPE } from "./messageCodec";

function buildBigData(channel: string, tSec: number, gen: number, kind: 0 | 1, payload: Uint8Array): ArrayBuffer {
  const chanBytes = new TextEncoder().encode(channel);
  const total = 1 + 4 + 8 + 1 + 2 + chanBytes.length + payload.length;
  const buf = new ArrayBuffer(total);
  const view = new DataView(buf);
  let o = 0;
  view.setUint8(o, BIGDATA_MESSAGE_TYPE); o += 1;
  view.setUint32(o, gen, true); o += 4;
  view.setFloat64(o, tSec, true); o += 8;
  view.setUint8(o, kind); o += 1;
  view.setUint16(o, chanBytes.length, true); o += 2;
  new Uint8Array(buf, o, chanBytes.length).set(chanBytes); o += chanBytes.length;
  new Uint8Array(buf, o, payload.length).set(payload);
  return buf;
}

describe("parseServerMessage BIGDATA", () => {
  it("解析图像大数据帧并分派 onBigData", () => {
    const payload = new Uint8Array([1, 2, 3, 4]);
    const buf = buildBigData("front_camera", 12.5, 7, 0, payload);
    let got: any = null;
    parseServerMessage(buf, { onBigData: (f) => { got = f; } });
    expect(got).not.toBeNull();
    expect(got.channel).toBe("front_camera");
    expect(got.tSec).toBeCloseTo(12.5);
    expect(got.gen).toBe(7);
    expect(got.kind).toBe("image");
    expect(Array.from(got.payload)).toEqual([1, 2, 3, 4]);
  });

  it("解析 rawData 大数据帧 kind=raw", () => {
    const buf = buildBigData("gnss", 3.0, 2, 1, new Uint8Array([9]));
    let got: any = null;
    parseServerMessage(buf, { onBigData: (f) => { got = f; } });
    expect(got.kind).toBe("raw");
    expect(got.channel).toBe("gnss");
  });
});