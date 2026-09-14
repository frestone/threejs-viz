import { Enum, Root, Type } from "protobufjs";
import type { Field } from "protobufjs";
import "protobufjs/ext/descriptor/index.js";
import type { RawDataDefs } from "../types";

const MAX_DEPTH = 64;
const MAX_FIELDS = 100_000;
const MAX_FIELD_NUMBER = 0x1fffffff;

type DescriptorRoot = Root & {
  fromDescriptor(descriptor: Uint8Array): Root;
};

interface CompiledSchema {
  error: string | null;
  root: Root | null;
}

export interface RawDataRegistry {
  schemas: Map<string, CompiledSchema>;
}

export interface DecodeTextResult {
  text: string;
  lines: string[];
  error?: string;
}

interface ParsedField {
  number: number;
  wireType: number;
  value: bigint | Uint8Array | ParsedField[];
}

interface ParseBudget {
  fields: number;
}

interface ParseResult {
  fields: ParsedField[];
  offset: number;
  endedGroup: boolean;
}

interface FormatCtx {
  lines: string[];
  chars: number;
}

export function buildRawDataRegistry(defs: RawDataDefs): RawDataRegistry {
  const schemas = new Map<string, CompiledSchema>();
  for (const schema of defs.schemas ?? []) {
    if (schema.encoding !== "protobuf") {
      schemas.set(schema.id, {
        error: `unsupported encoding "${schema.encoding}" for schema ${schema.id}（首期仅支持 protobuf）`,
        root: null,
      });
      continue;
    }
    schemas.set(schema.id, {
      error: null,
      root: parseDescriptorRoot(schema.dataBase64),
    });
  }
  return { schemas };
}

export function decodeText(
  registry: RawDataRegistry,
  schemaId: string,
  messageType: string,
  payload: Uint8Array,
): DecodeTextResult {
  const schema = registry.schemas.get(schemaId);
  if (!schema) return fail(`未知 schemaId: ${schemaId}`);
  if (schema.error) return fail(schema.error);
  if (!payload || payload.length === 0) return fail("空 payload，无可解码内容");

  try {
    const parsed = parseMessage(payload, 0, payload.length, 0, undefined, { fields: 0 });
    if (parsed.endedGroup || parsed.offset !== payload.length) {
      throw new Error("顶层消息包含意外的 end-group");
    }
    const ctx: FormatCtx = { lines: [], chars: 0 };
    const message = lookupMessageType(schema.root, messageType);
    formatFields(parsed.fields, 0, ctx, 0, message);
    const text = ctx.lines.join("\n");
    return { text, lines: ctx.lines };
  } catch (error) {
    return fail(`wire 解码失败: ${errMsg(error)}`);
  }
}

export function decodeDisplayText(
  registry: RawDataRegistry,
  schemaId: string,
  messageType: string,
  payload: Uint8Array,
): DecodeTextResult {
  const formatted = decodeTextFormat(registry, schemaId, messageType, payload);
  return formatted.error
    ? decodeText(registry, schemaId, messageType, payload)
    : formatted;
}

export function decodeTextFormat(
  registry: RawDataRegistry,
  schemaId: string,
  messageType: string,
  payload: Uint8Array,
): DecodeTextResult {
  const schema = registry.schemas.get(schemaId);
  if (!schema) return fail(`未知 schemaId: ${schemaId}`);
  if (schema.error) return fail(schema.error);
  if (!payload || payload.length === 0) return fail("空 payload，无可解码内容");
  const message = lookupMessageType(schema.root, messageType);
  if (!message) return fail(`Descriptor 中未找到消息类型: ${messageType}`);

  try {
    const parsed = parseMessage(payload, 0, payload.length, 0, undefined, { fields: 0 });
    if (parsed.endedGroup || parsed.offset !== payload.length) {
      throw new Error("顶层消息包含意外的 end-group");
    }
    const ctx: FormatCtx = { lines: [], chars: 0 };
    formatTextFields(parsed.fields, 0, ctx, 0, message);
    return { text: ctx.lines.join("\n"), lines: ctx.lines };
  } catch (error) {
    return fail(`TextFormat 解码失败: ${errMsg(error)}`);
  }
}

function parseMessage(
  bytes: Uint8Array,
  start: number,
  end: number,
  depth: number,
  expectedEndGroup: number | undefined,
  budget: ParseBudget,
): ParseResult {
  if (depth > MAX_DEPTH) throw new Error(`递归深度超过 ${MAX_DEPTH}`);
  const fields: ParsedField[] = [];
  let offset = start;

  while (offset < end) {
    const tag = readVarint(bytes, offset, end, "tag");
    offset = tag.offset;
    if (tag.value === 0n || tag.value > BigInt(0xffffffff)) {
      throw new Error("非法 tag");
    }
    const wireType = Number(tag.value & 7n);
    const fieldNumberBig = tag.value >> 3n;
    if (fieldNumberBig === 0n || fieldNumberBig > BigInt(MAX_FIELD_NUMBER)) {
      throw new Error("非法字段号");
    }
    const fieldNumber = Number(fieldNumberBig);

    if (wireType === 4) {
      if (expectedEndGroup === undefined) throw new Error("意外的 end-group");
      if (fieldNumber !== expectedEndGroup) throw new Error("group 字段号不匹配");
      return { fields, offset, endedGroup: true };
    }
    if (wireType === 6 || wireType === 7) throw new Error(`非法 wire type: ${wireType}`);
    budget.fields++;
    if (budget.fields > MAX_FIELDS) throw new Error(`字段数超过 ${MAX_FIELDS}`);

    if (wireType === 0) {
      const value = readVarint(bytes, offset, end, `field_${fieldNumber} varint`);
      offset = value.offset;
      fields.push({ number: fieldNumber, wireType, value: value.value });
    } else if (wireType === 1) {
      ensureAvailable(offset, 8, end, `field_${fieldNumber} fixed64`);
      fields.push({ number: fieldNumber, wireType, value: bytes.slice(offset, offset + 8) });
      offset += 8;
    } else if (wireType === 2) {
      const length = readVarint(bytes, offset, end, `field_${fieldNumber} length`);
      offset = length.offset;
      if (length.value > BigInt(end - offset)) {
        throw new Error(`field_${fieldNumber} length-delimited 越界`);
      }
      const size = Number(length.value);
      fields.push({ number: fieldNumber, wireType, value: bytes.slice(offset, offset + size) });
      offset += size;
    } else if (wireType === 3) {
      const group = parseMessage(bytes, offset, end, depth + 1, fieldNumber, budget);
      if (!group.endedGroup) throw new Error(`field_${fieldNumber} group 未闭合`);
      fields.push({ number: fieldNumber, wireType, value: group.fields });
      offset = group.offset;
    } else {
      ensureAvailable(offset, 4, end, `field_${fieldNumber} fixed32`);
      fields.push({ number: fieldNumber, wireType, value: bytes.slice(offset, offset + 4) });
      offset += 4;
    }
  }

  if (expectedEndGroup !== undefined) throw new Error(`field_${expectedEndGroup} group 未闭合`);
  return { fields, offset, endedGroup: false };
}

function readVarint(
  bytes: Uint8Array,
  start: number,
  end: number,
  label: string,
): { value: bigint; offset: number } {
  let value = 0n;
  for (let i = 0; i < 10; i++) {
    const offset = start + i;
    if (offset >= end) throw new Error(`${label} varint 截断`);
    const byte = bytes[offset];
    if (i === 9 && (byte & 0x80) !== 0) throw new Error(`${label} varint 超过 10 字节`);
    if (i === 9 && byte > 1) throw new Error(`${label} varint溢出 uint64`);
    value |= BigInt(byte & 0x7f) << BigInt(i * 7);
    if ((byte & 0x80) === 0) return { value, offset: offset + 1 };
  }
  throw new Error(`${label} varint 超过 10 字节`);
}

function ensureAvailable(offset: number, size: number, end: number, label: string): void {
  if (offset > end - size) throw new Error(`${label} 截断`);
}

function formatFields(
  fields: ParsedField[],
  indent: number,
  ctx: FormatCtx,
  depth: number,
  message?: Type,
): void {
  if (depth > MAX_DEPTH) throw new Error(`递归深度超过 ${MAX_DEPTH}`);
  for (const field of fields) {
    const descriptorField = message?.fieldsById[field.number];
    const name = descriptorField?.name ?? `field_${field.number}`;
    if (field.wireType === 0) {
      pushLine(ctx, indent, `${name} [varint]: ${(field.value as bigint).toString()}`);
    } else if (field.wireType === 1) {
      pushLine(ctx, indent, `${name} [fixed64]: ${littleEndianHex(field.value as Uint8Array)}`);
    } else if (field.wireType === 2) {
      const nestedType = descriptorField?.resolvedType instanceof Type
        ? descriptorField.resolvedType
        : undefined;
      formatLengthDelimited(name, field.value as Uint8Array, indent, ctx, depth, nestedType);
    } else if (field.wireType === 3) {
      pushLine(ctx, indent, `${name} [group] {`);
      const nestedType = descriptorField?.resolvedType instanceof Type
        ? descriptorField.resolvedType
        : undefined;
      formatFields(field.value as ParsedField[], indent + 1, ctx, depth + 1, nestedType);
      pushLine(ctx, indent, "}");
    } else {
      pushLine(ctx, indent, `${name} [fixed32]: ${littleEndianHex(field.value as Uint8Array)}`);
    }
  }
}

function formatLengthDelimited(
  name: string,
  bytes: Uint8Array,
  indent: number,
  ctx: FormatCtx,
  depth: number,
  message?: Type,
): void {
  pushLine(ctx, indent, `${name} [length-delimited]: hex:${hex(bytes)}`);
  const text = printableUtf8(bytes);
  if (text !== null) pushLine(ctx, indent + 1, `utf8: ${quoteString(text)}`);
  if (bytes.length === 0 || depth >= MAX_DEPTH) return;

  let nested: ParseResult;
  try {
    nested = parseMessage(bytes, 0, bytes.length, depth + 1, undefined, { fields: 0 });
  } catch {
    // 任意 bytes 都是合法 length-delimited；无法完整解析为消息时只展示原始 hex。
    return;
  }
  if (nested.fields.length === 0 || nested.offset !== bytes.length || nested.endedGroup) return;
  pushLine(ctx, indent + 1, "message {");
  formatFields(nested.fields, indent + 2, ctx, depth + 1, message);
  pushLine(ctx, indent + 1, "}");
}

function formatTextFields(
  fields: ParsedField[], indent: number, ctx: FormatCtx, depth: number, message: Type,
): void {
  if (depth > MAX_DEPTH) throw new Error(`递归深度超过 ${MAX_DEPTH}`);
  for (const parsed of fields) {
    const field = message.fieldsById[parsed.number];
    if (!field) {
      pushLine(ctx, indent, `field_${parsed.number}: ${wireFallback(parsed)}`);
      continue;
    }
    if (field.resolvedType instanceof Type) {
      const nested = parsed.wireType === 3 && Array.isArray(parsed.value)
        ? { fields: parsed.value, offset: 0, endedGroup: true }
        : parseNestedMessage(parsed, field, depth);
      pushLine(ctx, indent, `${field.name} {`);
      formatTextFields(nested.fields, indent + 1, ctx, depth + 1, field.resolvedType);
      pushLine(ctx, indent, "}");
      continue;
    }
    const values = unpackScalarField(parsed, field);
    for (const value of values) {
      pushLine(ctx, indent, `${field.name}: ${formatScalar(value, field)}`);
    }
  }
}

function parseNestedMessage(parsed: ParsedField, field: Field, depth: number): ParseResult {
  const bytes = requireBytes(parsed, field);
  const nested = parseMessage(bytes, 0, bytes.length, depth + 1, undefined, { fields: 0 });
  if (nested.endedGroup || nested.offset !== bytes.length) throw new Error(`${field.name} 嵌套消息未完整解析`);
  return nested;
}

function unpackScalarField(parsed: ParsedField, field: Field): ParsedField[] {
  if (!field.repeated || parsed.wireType !== 2 || !(parsed.value instanceof Uint8Array)) return [parsed];
  const wireType = scalarWireType(field);
  if (wireType === undefined) return [parsed];
  const bytes = parsed.value;
  const values: ParsedField[] = [];
  let offset = 0;
  while (offset < bytes.length) {
    if (wireType === 0) {
      const value = readVarint(bytes, offset, bytes.length, `${field.name} packed value`);
      values.push({ number: parsed.number, wireType, value: value.value });
      offset = value.offset;
    } else {
      const size = wireType === 1 ? 8 : 4;
      ensureAvailable(offset, size, bytes.length, `${field.name} packed value`);
      values.push({ number: parsed.number, wireType, value: bytes.slice(offset, offset + size) });
      offset += size;
    }
  }
  return values;
}

function scalarWireType(field: Field): 0 | 1 | 5 | undefined {
  if (field.resolvedType instanceof Enum) return 0;
  if (["double", "fixed64", "sfixed64"].includes(field.type)) return 1;
  if (["float", "fixed32", "sfixed32"].includes(field.type)) return 5;
  if (["int32", "int64", "uint32", "uint64", "sint32", "sint64", "bool"].includes(field.type)) return 0;
  return undefined;
}

function formatScalar(parsed: ParsedField, field: Field): string {
  if (field.resolvedType instanceof Enum) {
    const value = requireVarint(parsed, field);
    return field.resolvedType.valuesById[Number(value)] ?? value.toString();
  }
  switch (field.type) {
    case "string": return quoteString(new TextDecoder("utf-8", { fatal: true }).decode(requireBytes(parsed, field)));
    case "bytes": return quoteBytes(requireBytes(parsed, field));
    case "bool": return requireVarint(parsed, field) === 0n ? "false" : "true";
    case "sint32": return Number(zigZag(requireVarint(parsed, field))).toString();
    case "sint64": return zigZag(requireVarint(parsed, field)).toString();
    case "int32": return Number(BigInt.asIntN(32, requireVarint(parsed, field))).toString();
    case "int64": return BigInt.asIntN(64, requireVarint(parsed, field)).toString();
    case "uint32": return Number(BigInt.asUintN(32, requireVarint(parsed, field))).toString();
    case "uint64": return requireVarint(parsed, field).toString();
    case "double": return formatFloat(readNumber(requireFixed(parsed, field, 8), 8));
    case "float": return formatFloat(readNumber(requireFixed(parsed, field, 4), 4));
    case "fixed64": return readUnsigned(requireFixed(parsed, field, 8)).toString();
    case "sfixed64": return BigInt.asIntN(64, readUnsigned(requireFixed(parsed, field, 8))).toString();
    case "fixed32": return Number(readUnsigned(requireFixed(parsed, field, 4))).toString();
    case "sfixed32": return Number(BigInt.asIntN(32, readUnsigned(requireFixed(parsed, field, 4)))).toString();
    default: return requireVarint(parsed, field).toString();
  }
}

function requireVarint(parsed: ParsedField, field: Field): bigint {
  if (parsed.wireType !== 0 || typeof parsed.value !== "bigint") throw new Error(`${field.name} 的 wire type 与 ${field.type} 不匹配`);
  return parsed.value;
}

function requireBytes(parsed: ParsedField, field: Field): Uint8Array {
  if (parsed.wireType !== 2 || !(parsed.value instanceof Uint8Array)) throw new Error(`${field.name} 的 wire type 与 ${field.type} 不匹配`);
  return parsed.value;
}

function requireFixed(parsed: ParsedField, field: Field, size: 4 | 8): Uint8Array {
  if (parsed.wireType !== (size === 8 ? 1 : 5) || !(parsed.value instanceof Uint8Array)) throw new Error(`${field.name} 的 wire type 与 ${field.type} 不匹配`);
  return parsed.value;
}

function zigZag(value: bigint): bigint { return (value >> 1n) ^ -(value & 1n); }

function readUnsigned(bytes: Uint8Array): bigint {
  let value = 0n;
  for (let i = bytes.length - 1; i >= 0; i--) value = (value << 8n) | BigInt(bytes[i]);
  return value;
}

function readNumber(bytes: Uint8Array, size: 4 | 8): number {
  const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  return size === 8 ? view.getFloat64(0, true) : view.getFloat32(0, true);
}

function formatFloat(value: number): string {
  if (Number.isNaN(value)) return "nan";
  if (value === Infinity) return "inf";
  if (value === -Infinity) return "-inf";
  if (Object.is(value, -0)) return "-0";
  return value.toString();
}

function quoteBytes(bytes: Uint8Array): string {
  let value = '"';
  for (const byte of bytes) {
    if (byte === 0x22 || byte === 0x5c) value += `\\${String.fromCharCode(byte)}`;
    else if (byte === 0x0a) value += "\\n";
    else if (byte === 0x0d) value += "\\r";
    else if (byte === 0x09) value += "\\t";
    else if (byte >= 0x20 && byte <= 0x7e) value += String.fromCharCode(byte);
    else value += `\\${byte.toString(8).padStart(3, "0")}`;
  }
  return `${value}"`;
}

function wireFallback(field: ParsedField): string {
  if (typeof field.value === "bigint") return field.value.toString();
  if (field.value instanceof Uint8Array) return quoteBytes(field.value);
  return "{}";
}

function pushLine(ctx: FormatCtx, indent: number, content: string): void {
  const line = "  ".repeat(indent) + content;
  ctx.chars += line.length + 1;
  ctx.lines.push(line);
}

function littleEndianHex(bytes: Uint8Array): string {
  let value = 0n;
  for (let i = bytes.length - 1; i >= 0; i--) value = (value << 8n) | BigInt(bytes[i]);
  return `0x${value.toString(16).padStart(bytes.length * 2, "0")}`;
}

function hex(bytes: Uint8Array): string {
  let out = "0x";
  for (const byte of bytes) out += byte.toString(16).padStart(2, "0");
  return out;
}

function printableUtf8(bytes: Uint8Array): string | null {
  let text: string;
  try {
    text = new TextDecoder("utf-8", { fatal: true }).decode(bytes);
  } catch {
    return null;
  }
  for (const char of text) {
    const code = char.codePointAt(0) ?? 0;
    if (code < 0x20 || (code >= 0x7f && code <= 0x9f)) return null;
  }
  return text;
}

function quoteString(value: string): string {
  return JSON.stringify(value).replace(/[\u2028\u2029]/g, (char) =>
    `\\u${(char.codePointAt(0) ?? 0).toString(16).padStart(4, "0")}`,
  );
}

function encodeVarint(value: number): number[] {
  const bytes: number[] = [];
  do {
    let byte = value & 0x7f;
    value = Math.floor(value / 128);
    if (value !== 0) byte |= 0x80;
    bytes.push(byte);
  } while (value !== 0);
  return bytes;
}

function fileDescriptorName(bytes: Uint8Array): string | null {
  try {
    const descriptor = parseMessage(bytes, 0, bytes.length, 0, undefined, { fields: 0 });
    const name = descriptor.fields.find(
      (field) => field.number === 1 && field.wireType === 2 && field.value instanceof Uint8Array,
    );
    if (!(name?.value instanceof Uint8Array)) return null;
    const value = printableUtf8(name.value);
    return value?.endsWith(".proto") ? value : null;
  } catch {
    return null;
  }
}

function looksLikeFileDescriptorProto(bytes: Uint8Array): boolean {
  return fileDescriptorName(bytes) !== null;
}

function collectDependencyFiles(bytes: Uint8Array): Uint8Array[] {
  const container = parseMessage(bytes, 0, bytes.length, 0, undefined, { fields: 0 });
  const wrappedFile = container.fields.find(
    (field) => field.number === 2 && field.wireType === 2 &&
      field.value instanceof Uint8Array && looksLikeFileDescriptorProto(field.value),
  );
  if (wrappedFile?.value instanceof Uint8Array) {
    return [
      wrappedFile.value,
      ...container.fields.flatMap((field) =>
        field.number === 3 && field.wireType === 2 && field.value instanceof Uint8Array
          ? collectDependencyFiles(field.value)
          : [],
      ),
    ];
  }
  return looksLikeFileDescriptorProto(bytes) ? [bytes] : [];
}

function parseDescriptorRoot(dataBase64: string): Root | null {
  if (!dataBase64) return null;
  try {
    const binary = atob(dataBase64);
    const bytes = Uint8Array.from(binary, (char) => char.charCodeAt(0));
    const fromDescriptor = (Root as unknown as DescriptorRoot).fromDescriptor;

    // MCAP protobuf schema envelope：field 1 是 message type，field 2 是根
    // FileDescriptorProto；field 3 依赖既可能是文件，也可能是递归包装树。
    const envelope = parseMessage(bytes, 0, bytes.length, 0, undefined, { fields: 0 });
    const messageTypeField = envelope.fields.find(
      (field) => field.number === 1 && field.wireType === 2 && field.value instanceof Uint8Array,
    );
    const rootFile = envelope.fields.find(
      (field) => field.number === 2 && field.wireType === 2 && field.value instanceof Uint8Array,
    );
    if (messageTypeField && rootFile?.value instanceof Uint8Array) {
      const dependencyFiles = envelope.fields.flatMap((field) =>
        field.number === 3 && field.wireType === 2 && field.value instanceof Uint8Array
          ? collectDependencyFiles(field.value)
          : [],
      );
      const files = [rootFile.value, ...dependencyFiles];
      const uniqueFiles = [...new Map(
        files.flatMap((file) => {
          const name = fileDescriptorName(file);
          return name ? [[name, file] as const] : [];
        }),
      ).values()];
      const descriptorSet = Uint8Array.from(uniqueFiles.flatMap((file) => [
        0x0a,
        ...encodeVarint(file.length),
        ...file,
      ]));
      return fromDescriptor(descriptorSet);
    }

    return fromDescriptor(bytes);
  } catch {
    // Descriptor 只提供字段名元数据；无效时仍允许 wire-level 解码。
    return null;
  }
}

function lookupMessageType(root: Root | null, messageType: string): Type | undefined {
  if (!root || !messageType) return undefined;
  const normalized = messageType.startsWith(".") ? messageType.slice(1) : messageType;
  try {
    const object = root.lookup(normalized);
    return object instanceof Type ? object : undefined;
  } catch {
    return undefined;
  }
}

function fail(error: string): DecodeTextResult {
  return { text: "", lines: [], error };
}

function errMsg(error: unknown): string {
  return error instanceof Error ? error.message : String(error);
}