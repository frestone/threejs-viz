import { describe, it, expect } from "vitest";
import { FrameStream } from "./FrameStream";

// Task 3：FrameStream.reportPlayhead 应发送带当前代次的 playhead 上报。
// 用 fake ws 捕获 send 的 JSON 串。注：环境未装 vitest，本文件留待后续跑。
describe("FrameStream.reportPlayhead", () => {
  it("发送带当前代次的 playhead 上报", () => {
    const stream: any = new FrameStream("ws://x", {});
    const sent: string[] = [];
    stream.ws = { readyState: 1, send: (m: string) => sent.push(m) };
    (globalThis as any).WebSocket = { OPEN: 1 };
    stream.seek(10); // generation -> 1
    stream.reportPlayhead(12.5);
    const last = JSON.parse(sent[sent.length - 1]);
    expect(last.type).toBe("playhead");
    expect(last.timeSec).toBeCloseTo(12.5);
    expect(last.generation).toBe(1);
  });

  it("多次 seek 后上报携带最新代次", () => {
    const stream: any = new FrameStream("ws://x", {});
    const sent: string[] = [];
    stream.ws = { readyState: 1, send: (m: string) => sent.push(m) };
    (globalThis as any).WebSocket = { OPEN: 1 };
    stream.seek(1);
    stream.seek(2); // generation -> 2
    stream.reportPlayhead(3);
    const last = JSON.parse(sent[sent.length - 1]);
    expect(last.generation).toBe(2);
  });
});