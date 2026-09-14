import test from "node:test";
import assert from "node:assert/strict";
import { ConnectionAttempt } from "./connectionAttempt.ts";

test("取消后当前异步连接尝试失效", () => {
  const attempts = new ConnectionAttempt();
  const attempt = attempts.begin();

  attempts.cancel();

  assert.equal(attempts.isCurrent(attempt), false);
});

test("新的连接尝试会替换旧尝试", () => {
  const attempts = new ConnectionAttempt();
  const first = attempts.begin();
  const second = attempts.begin();

  assert.equal(attempts.isCurrent(first), false);
  assert.equal(attempts.isCurrent(second), true);
});