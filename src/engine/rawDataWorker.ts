// Task 6：RawData 解码 Web Worker（单 module Worker，串行处理）。
//
// 职责：把动态 protobuf registry 编译与 TextFormat 解码从主线程剥离，避免大 payload
// 在主线程同步解码阻塞 rAF 渲染。本 Worker 只有两类请求，串行处理（单线程天然串行，
// 保证 setDefinitions 一定先于依赖它的 decode 生效）：
//  - defs   ：收到最新 RawDataDefs，buildRawDataRegistry 建 registry 并持有（替换旧的）。
//  - decode ：用当前持有的 registry 调 decodeText，回包携带原 identity 元组。
//
// 复用 Task 5 的 rawDataCodec 纯函数（buildRawDataRegistry / decodeText），
// 不重复实现解码逻辑；identity（generation/channel/seq）原样回传，latest-wins判定
// 与 pending 管理由主线程 RawDataDecoder 负责，本 Worker 不做任何丢弃。
import {
  buildRawDataRegistry,
  decodeDisplayText,
  type RawDataRegistry,
} from "./rawDataCodec";
import type { RawDataDefs } from "../types";

// 主线程 → Worker：注册最新 definitions。
interface DefsRequest {
  type: "defs";
  defs: RawDataDefs;
}

// 主线程 → Worker：一条 decode 请求，携带 identity 与 payload 副本。
interface DecodeRequest {
  type: "decode";
  id: number;
  generation: number;
  channel: string;
  seq: number;
  schemaId: string;
  messageType: string;
  payload: Uint8Array;
}

type WorkerRequest = DefsRequest | DecodeRequest;

// Worker → 主线程：decode 回包，原样带回 identity 供主线程做 latest-wins 校验。
interface DecodeResponse {
  type: "decoded";
  id: number;
  generation: number;
  channel: string;
  seq: number;
  text: string;
  lines: string[];
  error?: string;
}

// 当前持有的 registry。defs 未到达前为 null，decode 直接回结构化错误。
let registry: RawDataRegistry | null = null;

function handleDefs(req: DefsRequest): void {
  registry = buildRawDataRegistry(req.defs);
}

function handleDecode(req: DecodeRequest): void {
  let text = "";
  let lines: string[] = [];
  let error: string | undefined;
  if (!registry) {
    error = "RawData definitions 尚未注册（需先 setDefinitions）";
  } else {
    const r = decodeDisplayText(registry, req.schemaId, req.messageType, req.payload);
    text = r.text;
    lines = r.lines;
    error = r.error;
  }
  const resp: DecodeResponse = {
    type: "decoded",
    id: req.id,
    generation: req.generation,
    channel: req.channel,
    seq: req.seq,
    text,
    lines,
    error,
  };
  (self as unknown as Worker).postMessage(resp);
}

self.onmessage = (ev: MessageEvent<WorkerRequest>) => {
  const req = ev.data;
  if (req.type === "defs") handleDefs(req);
  else if (req.type === "decode") handleDecode(req);
};