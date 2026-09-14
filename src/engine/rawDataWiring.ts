// Task 8：ThreeEngine 内部的 RawData 解码链路接线（可测的纯逻辑单元）。
//
// 把 type-12 defs → decoder.setDefinitions、已被 BigDataStore 接受的 type-11 raw 帧
// → decoder.decode → panelReducer → UI 通知 串成一条链路。为可测性从 threeEngine 抽出，
// 依赖注入 decoder / store / currentGeneration / onPanelUpdate。
//
// 硬约束体现：
//  - 一次最多一个 RawData 通道：subscribe 记录当前通道，换通道先 dispatch select 清旧。
//  - 订阅默认关闭：enabled_ 初值 false，未选通道/未开启时 onRawFrame 直接返回不 decode。
//  - 不 transfer store 持有 buffer：只把 entry.payload 引用传给 decode（decode 内部复制）。
//  - 错误隔离：decode reject / result.error 只 dispatch decodeFailed，绝不抛出、绝不影响渲染。
//  - generation 隔离：onClearCache 调 decoder.setGeneration + 清 panel；store 已做 gen 校验。
import type { RawDataDefs } from "../types";
import type { RawDataDecodeResult, RawDataDecodeRequest } from "./rawDataDecoder";
import {
  panelReducer,
  initialPanelState,
  type PanelState,
  type PanelAction,
} from "../rawDataPanelState";

// decoder 依赖面（真实为 RawDataDecoder，测试可注入 fake）。
export interface DecoderLike {
  setDefinitions(defs: RawDataDefs): void;
  setGeneration(generation: number): void;
  decode(req: RawDataDecodeRequest): Promise<RawDataDecodeResult>;
  dispose(): void;
}

// BigDataStore 当前帧读取面（只读取，不 transfer）。
export interface StoreLike {
  get(channel: string):
    | { tSec: number; gen: number; kind: "image" | "raw"; seq: number; payload: Uint8Array }
    | undefined;
}

export interface RawDataWiringOptions {
  decoder: DecoderLike;
  store: StoreLike;
  currentGeneration: () => number;
  isPaused?: () => boolean;
  // panel state 变化通知（对齐 onBigDataUpdate 回调模式，供 UI 拉取最新 state）。
  onPanelUpdate?: (state: PanelState) => void;
}

export interface RawDataWiring {
  handleDefs(defs: RawDataDefs): void;
  subscribe(channel: string, enabled: boolean): void;
  onRawFrame(channel: string): void;
  setPaused(paused: boolean): void;
  setVisible(visible: boolean): void;
  onClearCache(): void;
  // 面向 UI 的写入口：只接受纯 UI 交互 action（搜索/导航/视口），
  // 绝不接受 definitions/select/frame/decode*/reset 等链路内部 action。
  dispatchUi(action: RawDataUiAction): void;
  getState(): PanelState;
  dispose(): void;
}

// UI 可下发的 action 子集（类型收窄，防止组件绕过链路直接改内部状态）。
export type RawDataUiAction = Extract<
  PanelAction,
  { type: "query" }
    | { type: "nextMatch" }
    | { type: "previousMatch" }
    | { type: "setPage" }
    | { type: "setViewport" }
>;

export function createRawDataWiring(opts: RawDataWiringOptions): RawDataWiring {
  const { decoder, store, currentGeneration } = opts;

  let state: PanelState = initialPanelState;
  // 当前选中的 RawData 通道（null=未选）与订阅开关（默认关闭）。
  let selected: string | null = null;
  let enabled = false;
  // 最近一次 defs 快照：decode 时按 channel 查 messageType/schemaId。
  let defs: RawDataDefs | null = null;
  let disposed = false;
  let visible = true;
  // 避免“选择时读缓存”与紧随其后的同帧事件重复提交 Worker。
  // RawData wire seq 固定为 0，因此用时间和 payload 身份判同帧，并为解码链生成递增 identity。
  let lastDecodeKey: string | null = null;
  let lastDecodedPayload: Uint8Array | null = null;
  let rawDecodeSeq = 0;

  const dispatch = (action: Parameters<typeof panelReducer>[1]) => {
    const next = panelReducer(state, action);
    if (next !== state) {
      state = next;
      opts.onPanelUpdate?.(state);
    }
  };

  const channelDef = (channel: string) =>
    defs?.rawData.find((c) => c.id === channel);

  const decodeCurrent = (channel: string) => {
    // 面板隐藏 / 订阅关闭 / 非当前选中通道 / 播放暂停：不 decode。
    if (!visible || !enabled || channel !== selected || opts.isPaused?.()) return;
    const entry = store.get(channel);
    if (!entry || entry.kind !== "raw") return;
    const def = channelDef(channel);
    if (!def || !def.available || !def.schemaId || !def.messageType) return;
    // BigDataStore 已做 gen 校验才接受此帧。RawData 的 wire seq 固定为 0，
    // 不能据此去重；同一缓存帧的时间与 payload 引用均不变。
    const decodeKey = `${entry.gen}:${channel}:${entry.tSec}`;
    if (decodeKey === lastDecodeKey && entry.payload === lastDecodedPayload) return;
    lastDecodeKey = decodeKey;
    lastDecodedPayload = entry.payload;
    const seq = entry.seq || ++rawDecodeSeq;
    dispatch({ type: "frame", channel, seq });
    // 只传 payload 引用（decode 内部 slice 复制后 transfer 副本）——绝不 transfer store buffer。
    decoder
      .decode({
        generation: entry.gen,
        channel,
        seq,
        schemaId: def.schemaId,
        messageType: def.messageType,
        payload: entry.payload,
      })
      .then((result) => {
        if (disposed || !visible || opts.isPaused?.()) return;
        if (result.error) {
          dispatch({ type: "decodeFailed", channel, seq: result.seq, error: result.error });
        } else {
          dispatch({ type: "decodeSucceeded", result });
        }
      })
      .catch((err: unknown) => {
        // 错误隔离：只进 panel error，绝不冒泡影响渲染/播放。
        if (disposed || !visible || opts.isPaused?.()) return;
        const msg = err instanceof Error ? err.message : String(err);
        dispatch({ type: "decodeFailed", channel, seq, error: msg });
      });
  };

  return {
    handleDefs(newDefs) {
      if (disposed) return;
      defs = newDefs;
      lastDecodeKey = null;
      decoder.setDefinitions(newDefs);
      dispatch({ type: "definitions", defs: newDefs });
    },

    subscribe(channel, en) {
      if (disposed) return;
      if (!en) {
        // 关闭订阅：清当前选择与内容，保留可选通道列表。走 reducer 的 reset action
        // 保持单一真相源（panel state 唯一由 panelReducer 演进）。
        enabled = false;
        selected = null;
        lastDecodeKey = null;
        dispatch({ type: "reset" });
        return;
      }
      // 开启订阅并选中通道：选择变化时 reducer 的 select 会先清旧内容，随后立即
      // 尝试解码该通道已缓存的最新 protobuf 帧；无缓存时等待对应帧事件。
      enabled = true;
      if (channel !== selected) {
        selected = channel;
        dispatch({ type: "select", channel });
        decodeCurrent(channel);
      }
    },

    onRawFrame(channel) {
      if (disposed) return;
      decodeCurrent(channel);
    },

    setPaused(paused) {
      if (disposed || !paused) return;
      dispatch({ type: "cancelDecode" });
    },

    setVisible(nextVisible) {
   if (disposed || visible === nextVisible) return;
      visible = nextVisible;
      if (!visible) {
        lastDecodeKey = null;
        dispatch({ type: "cancelDecode" });
      } else if (selected) {
        decodeCurrent(selected);
      }
    },

    onClearCache() {
      if (disposed) return;
      decoder.setGeneration(currentGeneration());
      // 清 panel 内容但保留可选通道列表（defs 不随 clearCache 消失）。走 reducer reset。
      selected = null;
      enabled = false;
      lastDecodeKey = null;
      dispatch({ type: "reset" });
    },

    dispatchUi(action) {
      // 仅转发 UI action（类型已在接口层收窄），复用统一 dispatch 走 reducer。
      if (disposed) return;
      dispatch(action);
    },

    getState() {
      return state;
    },

    dispose() {
      if (disposed) return;
      disposed = true;
      decoder.dispose();
    },
  };
}