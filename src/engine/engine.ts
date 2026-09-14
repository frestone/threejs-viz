// 引擎入口：three→stream→mock 级联（替代原 wasm→stream→mock）。
// three 引擎为默认渲染实现；若 Three.js/WebGL 初始化或 proto 加载失败，回退到
// 纯 stream（仅播放控制，不渲染）；再失败回退 mock（本地自驱动，无后端）。
import type { EngineApi } from "../types";
import { createThreeEngine, type ThreeEngineOptions } from "./threeEngine";

export type EngineKind = "three" | "stream" | "mock";

export async function createEngine(
  kind: EngineKind,
  opts: ThreeEngineOptions
): Promise<{ engine: EngineApi; kind: EngineKind }> {
  if (kind === "three") {
    return { engine: await createThreeEngine(opts), kind };
  }
  if (kind === "stream") {
    // 纯数据流：复用 threeEngine 但不渲染的场景较少见，这里直接退回 mock 语义
    // 由调用方级联决定；stream 档位保留以兼容原接口，实际走 three 装配失败分支。
    const { createMockEngine } = await import("./mockEngine");
    return { engine: createMockEngine(), kind: "mock" };
  }
  const { createMockEngine } = await import("./mockEngine");
  return { engine: createMockEngine(), kind: "mock" };
}