import { describe, expect, it } from "vitest";
import type { RawDataDefs } from "../types";
import {
  buildRawDataRegistry,
  decodeDisplayText,
  decodeText,
  decodeTextFormat,
} from "./rawDataCodec";

function defs(encoding = "protobuf"): RawDataDefs {
  return {
    rawData: [{
      id: "channel",
      topic: "/raw",
      label: "Raw",
      available: true,
      messageType: "ignored.Type",
      schemaId: "1",
    }],
    schemas: [{ id: "1", encoding: encoding as "protobuf", dataBase64: "" }],
  };
}

function varint(value: bigint): number[] {
  const bytes: number[] = [];
  do {
    let byte = Number(value & 0x7fn);
    value >>= 7n;
    if (value !== 0n) byte |= 0x80;
    bytes.push(byte);
  } while (value !== 0n);
  return bytes;
}

function tag(field: number, wireType: number): number[] {
  return varint(BigInt(field * 8 + wireType));
}

function fieldVarint(field: number, value: bigint): number[] {
  return [...tag(field, 0), ...varint(value)];
}

function fieldBytes(field: number, value: number[]): number[] {
  return [...tag(field, 2), ...varint(BigInt(value.length)), ...value];
}

function bytesBase64(bytes: number[]): string {
  let binary = "";
  for (const byte of bytes) binary += String.fromCharCode(byte);
  return btoa(binary);
}

function descriptorDefs(): RawDataDefs {
  const stringField = (field: number, value: string) =>
    fieldBytes(field, Array.from(new TextEncoder().encode(value)));
  const messageField = (field: number, value: number[]) => fieldBytes(field, value);
  const scalarField = (name: string, number: number, type: bigint) => [
    ...stringField(1, name),
    ...fieldVarint(3, BigInt(number)),
    ...fieldVarint(4, 1n),
    ...fieldVarint(5, type),
  ];
  const speedField = scalarField("speed", 1, 4n);
  const engineField = [
    ...stringField(1, "engine"),
    ...fieldVarint(3, 2n),
    ...fieldVarint(4, 1n),
    ...fieldVarint(5, 11n),
    ...stringField(6, ".pkg.Engine"),
  ];
  const vehicle = [
    ...stringField(1, "Vehicle"),
    ...messageField(2, speedField),
    ...messageField(2, engineField),
  ];
  const engine = [
    ...stringField(1, "Engine"),
    ...messageField(2, scalarField("rpm", 1, 13n)),
  ];
  const file = [
    ...stringField(1, "vehicle.proto"),
    ...stringField(2, "pkg"),
    ...messageField(4, vehicle),
    ...messageField(4, engine),
  ];
  const descriptorSet = messageField(1, file);
  const value = defs();
  value.rawData[0].messageType = "pkg.Vehicle";
  value.schemas[0].dataBase64 = bytesBase64(descriptorSet);
  return value;
}

function textFormatDefs(): RawDataDefs {
  const stringField = (field: number, value: string) =>
    fieldBytes(field, Array.from(new TextEncoder().encode(value)));
  const messageField = (field: number, value: number[]) => fieldBytes(field, value);
  const scalarField = (name: string, number: number, type: bigint, label = 1n, typeName?: string) => [
    ...stringField(1, name), ...fieldVarint(3, BigInt(number)),
    ...fieldVarint(4, label), ...fieldVarint(5, type),
    ...(typeName ? stringField(6, typeName) : []),
  ];
  const nested = [
    ...stringField(1, "Nested"),
    ...messageField(2, scalarField("name", 1, 9n)),
  ];
  const sample = [
    ...stringField(1, "Sample"),
    ...messageField(2, scalarField("signed", 1, 17n)),
    ...messageField(2, scalarField("ratio", 2, 1n)),
    ...messageField(2, scalarField("label", 3, 9n)),
    ...messageField(2, scalarField("blob", 4, 12n)),
    ...messageField(2, scalarField("nested", 5, 11n, 1n, ".pkg.Nested")),
    ...messageField(2, scalarField("samples", 6, 13n, 3n)),
  ];
  const file = [
    ...stringField(1, "sample.proto"), ...stringField(2, "pkg"),
    ...messageField(4, sample), ...messageField(4, nested),
  ];
  const value = defs();
  value.rawData[0].messageType = "pkg.Sample";
  value.schemas[0].dataBase64 = bytesBase64(messageField(1, file));
  return value;
}

function fixed64(value: number): number[] {
  const bytes = new Uint8Array(8);
  new DataView(bytes.buffer).setFloat64(0, value, true);
  return Array.from(bytes);
}

function decode(payload: number[]) {
  return decodeText(buildRawDataRegistry(defs()), "1", "anything", Uint8Array.from(payload));
}

describe("raw protobuf wire text decoder", () => {
  it("descriptor 缺失或 messageType 未知时回退字段号", () => {
    const registry = buildRawDataRegistry(defs());
    const result = decodeText(registry, "1", "No.Descriptor.Type", Uint8Array.from(fieldVarint(1, 7n)));

    expect(result).toEqual({
      text: "field_1 [varint]: 7",
      lines: ["field_1 [varint]: 7"],
    });
  });

  it("使用 DescriptorSet 和 messageType 显示真实字段名", () => {
    const registry = buildRawDataRegistry(descriptorDefs());
    const result = decodeText(registry, "1", ".pkg.Vehicle", Uint8Array.from(fieldVarint(1, 7n)));

    expect(result).toEqual({
      text: "speed [varint]: 7",
      lines: ["speed [varint]: 7"],
    });
  });

  it("未知字段回退字段号，并为声明的嵌套消息传播字段名", () => {
    const registry = buildRawDataRegistry(descriptorDefs());
    const nested = fieldVarint(1, 3200n);
    const result = decodeText(registry, "1", "pkg.Vehicle", Uint8Array.from([
      ...fieldBytes(2, nested),
      ...fieldVarint(9, 7n),
    ]));

    expect(result.lines).toEqual([
      "engine [length-delimited]: hex:0x088019",
      "  message {",
      "    rpm [varint]: 3200",
      "  }",
      "field_9 [varint]: 7",
    ]);
  });

  it("支持 MCAP protobuf schema envelope 中的 FileDescriptorProto", () => {
    const value = descriptorDefs();
    const descriptorSet = Array.from(
      atob(value.schemas[0].dataBase64),
      (char) => char.charCodeAt(0),
    );
    let fileOffset = 1; // FileDescriptorSet.file 的 tag
    while ((descriptorSet[fileOffset++] & 0x80) !== 0) {
      // 跳过 FileDescriptorProto 长度 varint。
    }
    const fileDescriptor = descriptorSet.slice(fileOffset);
    const envelope = [
      ...fieldBytes(1, Array.from(new TextEncoder().encode("pkg.Vehicle"))),
      ...fieldBytes(2, fileDescriptor),
    ];
    value.schemas[0].dataBase64 = bytesBase64(envelope);

    const result = decodeText(
      buildRawDataRegistry(value),
      "1",
      "pkg.Vehicle",
      Uint8Array.from(fieldVarint(1, 7n)),
    );

    expect(result.lines).toEqual(["speed [varint]: 7"]);
  });

  it("支持 MCAP envelope 对依赖 FileDescriptorProto 的二次封装", () => {
    const stringField = (field: number, value: string) =>
      fieldBytes(field, Array.from(new TextEncoder().encode(value)));
    const messageField = (field: number, value: number[]) => fieldBytes(field, value);
    const engineRef = [
      ...stringField(1, "engine"), ...fieldVarint(3, 1n), ...fieldVarint(4, 1n),
      ...fieldVarint(5, 11n), ...stringField(6, ".dep.Engine"),
    ];
    const rootFile = [
      ...stringField(1, "vehicle.proto"), ...stringField(2, "pkg"),
      ...stringField(3, "engine.proto"),
      ...messageField(4, [...stringField(1, "Vehicle"), ...messageField(2, engineRef)]),
    ];
    const rpmField = [
      ...stringField(1, "rpm"), ...fieldVarint(3, 1n),
      ...fieldVarint(4, 1n), ...fieldVarint(5, 13n),
    ];
    const dependencyFile = [
      ...stringField(1, "engine.proto"), ...stringField(2, "dep"),
      ...messageField(4, [...stringField(1, "Engine"), ...messageField(2, rpmField)]),
    ];
    const value = defs();
    value.schemas[0].dataBase64 = bytesBase64([
      ...fieldBytes(1, Array.from(new TextEncoder().encode("pkg.Vehicle"))),
      ...fieldBytes(2, rootFile),
      ...fieldBytes(3, fieldBytes(2, dependencyFile)),
    ]);

    const result = decodeText(
      buildRawDataRegistry(value), "1", "pkg.Vehicle",
      Uint8Array.from(fieldBytes(1, fieldVarint(1, 3200n))),
    );

    expect(result.lines).toContain("engine [length-delimited]: hex:0x088019");
    expect(result.lines).toContain("    rpm [varint]: 3200");
  });

  it("无效 DescriptorSet 不阻断手写 wire 解码", () => {
    const value = defs();
    value.schemas[0].dataBase64 = "not valid base64!";
    const result = decodeText(buildRawDataRegistry(value), "1", "pkg.Vehicle", Uint8Array.from(fieldVarint(1, 7n)));

    expect(result.lines).toEqual(["field_1 [varint]: 7"]);
  });

  it("解析 wire types 0、1、2、5 并只使用 field number", () => {
    const payload = [
      ...fieldVarint(1, 150n),
      ...tag(2, 1), 0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01,
      ...fieldBytes(3, [0xff, 0x00, 0x41]),
      ...tag(4, 5), 0x78, 0x56, 0x34, 0x12,
    ];

    expect(decode(payload).lines).toEqual([
      "field_1 [varint]: 150",
      "field_2 [fixed64]: 0x0102030405060708",
      "field_3 [length-delimited]: hex:0xff0041",
      "field_4 [fixed32]: 0x12345678",
    ]);
  });

  it("用 BigInt 精确输出 uint64 最大值", () => {
    expect(decode(fieldVarint(1, 18_446_744_073_709_551_615n)).text)
      .toBe("field_1 [varint]: 18446744073709551615");
  });

  it("length-delimited 始终输出原始 hex，并额外识别严格可打印 UTF-8", () => {
    const result = decode(fieldBytes(2, Array.from(new TextEncoder().encode("hello世界"))));

    expect(result.lines).toEqual([
      "field_2 [length-delimited]: hex:0x68656c6c6fe4b896e7958c",
      "  utf8: \"hello世界\"",
    ]);
    expect(decode(fieldBytes(2, [0x0a])).lines).toEqual([
      "field_2 [length-delimited]: hex:0x0a",
    ]);
  });

  it("仅在完整消费 length-delimited 内容时额外展示可能的嵌套消息", () => {
    const nested = [...fieldVarint(1, 42n), ...fieldBytes(2, [0xff])];
    const result = decode(fieldBytes(3, nested));

    expect(result.lines).toEqual([
      "field_3 [length-delimited]: hex:0x082a1201ff",
      "  message {",
      "    field_1 [varint]: 42",
      "    field_2 [length-delimited]: hex:0xff",
      "  }",
    ]);
  });

  it("解析匹配的 start-group/end-group（wire types 3/4）", () => {
    const result = decode([
      ...tag(5, 3),
      ...fieldVarint(1, 9n),
      ...tag(5, 4),
    ]);

    expect(result.lines).toEqual([
      "field_5 [group] {",
      "  field_1 [varint]: 9",
      "}",
    ]);
  });

  it("截断 varint 返回结构化错误且不泄漏部分输出", () => {
    const result = decode([...tag(1, 0), 0x80]);

    expect(result.error).toMatch(/varint.*截断/);
    expect(result.text).toBe("");
    expect(result.lines).toEqual([]);
  });

  it("length-delimited 声明长度越界返回结构化错误", () => {
    const result = decode([...tag(1, 2), 0x05, 0x41]);

    expect(result.error).toMatch(/越界/);
    expect(result.lines).toEqual([]);
  });

  it.each([6, 7])("拒绝非法 wire type %i", (wireType: number) => {
    const result = decode(tag(1, wireType));

    expect(result.error).toContain(`非法 wire type: ${wireType}`);
  });

  it("拒绝零 tag、孤立 end-group 和字段号不匹配的 group", () => {
    expect(decode([0]).error).toMatch(/非法 tag/);
    expect(decode(tag(1, 4)).error).toMatch(/意外的 end-group/);
    expect(decode([...tag(1, 3), ...tag(2, 4)]).error).toMatch(/group 字段号不匹配/);
  });

  it("fixed32/fixed64 截断返回结构化错误", () => {
    expect(decode([...tag(1, 5), 1, 2, 3]).error).toMatch(/fixed32.*截断/);
    expect(decode([...tag(1, 1), 1, 2, 3]).error).toMatch(/fixed64.*截断/);
  });

  it("拒绝 varint 超长、uint64 溢出和未闭合 group", () => {
    expect(decode([...tag(1, 0), ...Array(10).fill(0x80), 0]).error).toMatch(/超过 10 字节/);
    expect(decode([...tag(1, 0), ...Array(9).fill(0x80), 0x02]).error).toMatch(/溢出 uint64/);
    expect(decode(tag(1, 3)).error).toMatch(/group 未闭合/);
  });

  it("拒绝超过递归深度限制的 group", () => {
    const payload: number[] = [];
    for (let i = 0; i < 65; i++) payload.push(...tag(1, 3));
    for (let i = 0; i < 65; i++) payload.push(...tag(1, 4));

    expect(decode(payload).error).toMatch(/递归深度超过 64/);
  });

  it("空 payload、未知 schema 和不支持 encoding 返回结构化错误", () => {
    const registry = buildRawDataRegistry(defs());
    expect(decodeText(registry, "1", "T", new Uint8Array()).error).toMatch(/空 payload/);
    expect(decodeText(registry, "missing", "T", Uint8Array.of(8, 1)).error).toMatch(/未知 schemaId/);

    const unsupported = buildRawDataRegistry(defs("json"));
    expect(decodeText(unsupported, "1", "T", Uint8Array.of(8, 1)).error).toMatch(/unsupported encoding/);
  });

  it("允许完整输出超过旧的 2000000 字符限制", () => {
    const valueSize = 1_000_000;
    const length = varint(BigInt(valueSize));
    const payload = new Uint8Array(1 + length.length + valueSize);
    payload[0] = tag(1, 2)[0];
    payload.set(length, 1);

    const result = decodeText(buildRawDataRegistry(defs()), "1", "anything", payload);

    expect(result.error).toBeUndefined();
    expect(result.text.length).toBeGreaterThan(2_000_000);
    expect(result.lines).toHaveLength(1);
  });

  it("重复解码输出稳定并保留 wire 中的重复字段顺序", () => {
    const payload = [...fieldVarint(2, 2n), ...fieldVarint(1, 1n), ...fieldVarint(2, 3n)];
    const first = decode(payload);
    const second = decode(payload);

    expect(first).toEqual(second);
    expect(first.lines).toEqual([
      "field_2 [varint]: 2",
      "field_1 [varint]: 1",
      "field_2 [varint]: 3",
    ]);
  });
  it("按 Descriptor 输出 TextFormat 标量、嵌套消息和 packed repeated", () => {
    const payload = [
      ...fieldVarint(1, 9n),
      ...tag(2, 1), ...fixed64(3.5),
      ...fieldBytes(3, Array.from(new TextEncoder().encode("hello"))),
      ...fieldBytes(4, [0x00, 0x22, 0xff]),
      ...fieldBytes(5, fieldBytes(1, Array.from(new TextEncoder().encode("child")))),
      ...fieldBytes(6, [...varint(1n), ...varint(150n)]),
    ];

    const result = decodeTextFormat(
      buildRawDataRegistry(textFormatDefs()), "1", "pkg.Sample", Uint8Array.from(payload),
    );

    expect(result.error).toBeUndefined();
    expect(result.lines).toEqual([
      "signed: -5",
      "ratio: 3.5",
      "label: \"hello\"",
      "blob: \"\\000\\\"\\377\"",
      "nested {",
      "  name: \"child\"",
      "}",
      "samples: 1",
      "samples: 150",
    ]);
  });

  it("前端显示优先使用 TextFormat，Descriptor 不可用时降级 Wire", () => {
    const payload = Uint8Array.from([
      ...fieldVarint(1, 9n),
      ...fieldBytes(5, fieldBytes(1, Array.from(new TextEncoder().encode("child")))),
    ]);
    const registry = buildRawDataRegistry(textFormatDefs());

    const formatted = decodeDisplayText(registry, "1", "pkg.Sample", payload);
    expect(formatted.error).toBeUndefined();
    expect(formatted.lines).toEqual([
      "signed: -5",
      "nested {",
      "  name: \"child\"",
      "}",
    ]);
    expect(formatted.text).not.toContain("[length-delimited]: hex:");

    const fallback = decodeDisplayText(registry, "1", "pkg.Missing", payload);
    expect(fallback.error).toBeUndefined();
    expect(fallback.text).toContain("[length-delimited]");
  });
});