import { describe, it, expect } from "vitest";
import {
  parseServerMessage,
  RAW_DATA_DEFS_MESSAGE_TYPE,
  CHART_DEFS_MESSAGE_TYPE,
} from "./messageCodec";
import type { RawDataDefs } from "../../types";

// 将任意对象编码成 type 12 封包：[type:u8=12][UTF-8 JSON]。
function buildRawDataDefs(obj: unknown): ArrayBuffer {
  const jsonBytes = new TextEncoder().encode(JSON.stringify(obj));
  const buf = new ArrayBuffer(1 + jsonBytes.length);
  const view = new DataView(buf);
  view.setUint8(0, RAW_DATA_DEFS_MESSAGE_TYPE);
  new Uint8Array(buf, 1).set(jsonBytes);
  return buf;
}

// 构造合法的最小 base64（"AAAA" -> 3 字节 0）。
const VALID_B64 = "AAAA";

describe("parseServerMessage RAW_DATA_DEFS (type 12)", () => {
  it("解析合法完整快照并分派 onRawDataDefs", () => {
    const payload = {
      rawData: [
        {
          id: "planning/trajectory",
          topic: "/planning/trajectory",
          label: "规划轨迹",
          available: true,
          messageType: "apollo.planning.ADCTrajectory",
          schemaId: "7",
        },
        {
          id: "sensor/lidar",
          topic: "/sensor/lidar",
          label: "激光雷达",
          available: false,
          unavailableReason: "无匹配 schema",
        },
      ],
      schemas: [{ id: "7", encoding: "protobuf", dataBase64: VALID_B64 }],
    };
    const buf = buildRawDataDefs(payload);
    let got: RawDataDefs | null = null;
    let err: unknown = null;
    parseServerMessage(buf, {
      onRawDataDefs: (d) => { got = d; },
      onError: (e) => { err = e; },
    });
    expect(err).toBeNull();
    expect(got).not.toBeNull();
    const defs = got as unknown as RawDataDefs;
    expect(defs.rawData).toHaveLength(2);
    // available=true 通道字段完整且 schemaId 保持 string。
    expect(defs.rawData[0].available).toBe(true);
    expect(defs.rawData[0].messageType).toBe("apollo.planning.ADCTrajectory");
    expect(defs.rawData[0].schemaId).toBe("7");
    expect(typeof defs.rawData[0].schemaId).toBe("string");
    // available=false 通道带原因、无 messageType/schemaId。
    expect(defs.rawData[1].available).toBe(false);
    expect(defs.rawData[1].unavailableReason).toBe("无匹配 schema");
    expect(defs.rawData[1].messageType).toBeUndefined();
    // schema id 保持 string，dataBase64 原样透传。
    expect(defs.schemas).toHaveLength(1);
    expect(defs.schemas[0].id).toBe("7");
    expect(typeof defs.schemas[0].id).toBe("string");
    expect(defs.schemas[0].encoding).toBe("protobuf");
    expect(defs.schemas[0].dataBase64).toBe(VALID_B64);
    // 外层不得解析出 generation 字段。
    expect((defs as unknown as { generation?: unknown }).generation).toBeUndefined();
  });

  it("容忍未知多余字段", () => {
    const payload = {
      extraTop: 123,
      rawData: [
        {
          id: "c1", topic: "/t", label: "L", available: true,
          messageType: "M", schemaId: "1", extraCh: "x",
        },
      ],
      schemas: [{ id: "1", encoding: "protobuf", dataBase64: VALID_B64, extraS: 1 }],
    };
    const buf = buildRawDataDefs(payload);
    let got: RawDataDefs | null = null;
    let err: unknown = null;
    parseServerMessage(buf, {
      onRawDataDefs: (d) => { got = d; },
      onError: (e) => { err = e; },
    });
    expect(err).toBeNull();
    expect(got).not.toBeNull();
  });

  it("rawData 非数组 → 拒绝，不触发回调", () => {
    const buf = buildRawDataDefs({ rawData: {}, schemas: [] });
    let got: RawDataDefs | null = null;
    let err: unknown = null;
    parseServerMessage(buf, {
      onRawDataDefs: (d) => { got = d; },
      onError: (e) => { err = e; },
    });
    expect(got).toBeNull();
    expect(err).toBeInstanceOf(Error);
  });

  it("available 非 boolean → 拒绝", () => {
    const buf = buildRawDataDefs({
      rawData: [{ id: "c", topic: "/t", label: "L", available: "yes" }],
      schemas: [],
    });
    let got: RawDataDefs | null = null;
    let err: unknown = null;
    parseServerMessage(buf, {
      onRawDataDefs: (d) => { got = d; },
      onError: (e) => { err = e; },
    });
    expect(got).toBeNull();
    expect(err).toBeInstanceOf(Error);
  });

  it("id 为 number → 拒绝", () => {
    const buf = buildRawDataDefs({
      rawData: [{ id: 5, topic: "/t", label: "L", available: false }],
      schemas: [],
    });
    let got: RawDataDefs | null = null;
    let err: unknown = null;
    parseServerMessage(buf, {
      onRawDataDefs: (d) => { got = d; },
      onError: (e) => { err = e; },
    });
    expect(got).toBeNull();
    expect(err).toBeInstanceOf(Error);
  });

  it("available=true 缺 schemaId → 拒绝", () => {
    const buf = buildRawDataDefs({
      rawData: [{ id: "c", topic: "/t", label: "L", available: true, messageType: "M" }],
      schemas: [{ id: "1", encoding: "protobuf", dataBase64: VALID_B64 }],
    });
    let got: RawDataDefs | null = null;
    let err: unknown = null;
    parseServerMessage(buf, {
      onRawDataDefs: (d) => { got = d; },
      onError: (e) => { err = e; },
    });
    expect(got).toBeNull();
    expect(err).toBeInstanceOf(Error);
  });

  it("schema encoding 非 protobuf → 拒绝", () => {
    const buf = buildRawDataDefs({
      rawData: [],
      schemas: [{ id: "1", encoding: "json", dataBase64: VALID_B64 }],
    });
   let got: RawDataDefs | null = null;
    let err: unknown = null;
    parseServerMessage(buf, {
      onRawDataDefs: (d) => { got = d; },
      onError: (e) => { err = e; },
    });
    expect(got).toBeNull();
    expect(err).toBeInstanceOf(Error);
  });

  it("非法 base64 → 拒绝", () => {
    const buf = buildRawDataDefs({
      rawData: [],
      schemas: [{ id: "1", encoding: "protobuf", dataBase64: "@@@not-base64@@@" }],
    });
    let got: RawDataDefs | null = null;
    let err: unknown = null;
    parseServerMessage(buf, {
      onRawDataDefs: (d) => { got = d; },
      onError: (e) => { err = e; },
    });
    expect(got).toBeNull();
    expect(err).toBeInstanceOf(Error);
  });

  it("其他消息类型(type 6)不得触发 onRawDataDefs", () => {
    const jsonBytes = new TextEncoder().encode(JSON.stringify({ charts: [] }));
    const buf = new ArrayBuffer(1 + jsonBytes.length);
    const view = new DataView(buf);
    view.setUint8(0, CHART_DEFS_MESSAGE_TYPE);
    new Uint8Array(buf, 1).set(jsonBytes);
    let fired = false;
    parseServerMessage(buf, {
      onRawDataDefs: () => { fired = true; },
    });
    expect(fired).toBe(false);
  });
});