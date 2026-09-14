import test from "node:test";
import assert from "node:assert/strict";
import { resolveTransportMode } from "./transportMode.ts";

test("桌面环境选择 FFI 传输", () => {
  assert.equal(resolveTransportMode(true), "ffi");
});

test("浏览器环境保留 WebSocket 传输", () => {
  assert.equal(resolveTransportMode(false), "ws");
});

test("显式模式优先于环境自动判断", () => {
  assert.equal(resolveTransportMode(true, "ws"), "ws");
});