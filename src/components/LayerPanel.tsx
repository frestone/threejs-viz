import { useState } from "react";
import { type ImageChannelDef, type LayerDef } from "../types";

export interface LayerPanelProps {
  visible: Record<string, boolean>;
  onToggle: (layerId: string, visible: boolean) => void;
  // 服务端下发的图层定义（唯一配置来源 decoder*.json 的 layers）。图层的分组
  // （group/groupLabel）与展示名（label）随定义一起下发，前端��此动态建树，
  // 不再依赖 public/layer_tree.json，也不硬编码图层列表。
  layerDefs?: LayerDef[];
  // 相机图像通道（唯一来源 decoder*.json 的 imageChannels，随 IMAGE_DEFS 下发），
  // 渲染在“图像”组内。默认不勾选、不订阅；勾选后才 subscribeImage（零成本）。
  imageDefs?: ImageChannelDef[];
  cameraOn?: Set<string>;
  onToggleCamera?: (channel: string, enabled: boolean) => void;
}

// 分组节点的勾选态：全选=checked，全不选=unchecked，部分=indeterminate。
function groupState(
  ids: string[],
  visible: Record<string, boolean>
): "all" | "none" | "partial" {
  const on = ids.filter((id) => visible[id]).length;
  if (on === 0) return "none";
  if (on === ids.length) return "all";
  return "partial";
}

// 分组显示顺序：预定义分组优先按此顺序，未预定义的分组按首次出现顺序追加在后。
const GROUP_ORDER = ["ego", "planning", "perception", "prediction", "map"];

// 一个分组内的一个可勾选条目：单图层叶子，或聚合多图层的“合并叶子”（如边界线）。
interface LeafItem {
  key: string;    // React key
  label: string;  // 展示名
  ids: string[];  // 该条目控制的图层 id 列表（单图层长度为 1）
}

interface GroupBucket {
  id: string;        // 分组 id（group 字段）
  label: string;     // 分组展示名（groupLabel，回退 id）
  leaves: LeafItem[];
}

// 从下发的 layerDefs 动态构建分组树：
// - 按 group 字段分桶（无 group 的归入 "map" 兜底组）；
// - 组内 boundary_* 前缀的图层聚合为单个“边界线”条目（显隐统一控制）；
// - 其余图层各自成叶子，展示名取 label 回退 id。
function buildGroups(layerDefs: LayerDef[]): GroupBucket[] {
  const order: string[] = [];
  const map = new Map<string, GroupBucket>();

  for (const d of layerDefs) {
    const gid = d.group || "map";
    if (!map.has(gid)) {
      map.set(gid, { id: gid, label: d.groupLabel || gid, leaves: [] });
      order.push(gid);
    }
    const bucket = map.get(gid)!;
    if (!bucket.label && d.groupLabel) bucket.label = d.groupLabel;
  }

  // 逐组填充叶子：先分离 boundary_* 前缀图层，聚合成单个“边界线”条目。
  for (const gid of order) {
    const bucket = map.get(gid)!;
    const defs = layerDefs.filter((d) => (d.group || "map") === gid);
    const boundaryIds = defs
      .map((d) => d.id)
      .filter((id)=> id === "boundary" || id.startsWith("boundary_"));
    for (const d of defs) {
      const id = d.id;
      if (id === "boundary" || id.startsWith("boundary_")) continue;
      bucket.leaves.push({ key: id, label: d.label || id, ids: [id] });
    }
    if (boundaryIds.length > 0) {
      bucket.leaves.push({ key: "boundary_all", label: "边界线", ids: boundaryIds });
    }
  }

  // 排序：预定义分组在前（按 GROUP_ORDER），其余按首次出现顺序追加。
  return order
    .map((gid) => map.get(gid)!)
    .sort((a, b) => {
      const ia = GROUP_ORDER.indexOf(a.id);
      const ib = GROUP_ORDER.indexOf(b.id);
      const ra = ia === -1 ? GROUP_ORDER.length + order.indexOf(a.id) : ia;
      const rb = ib === -1 ? GROUP_ORDER.length + order.indexOf(b.id) : ib;
      return ra - rb;
    });
}

export function LayerPanel(props: LayerPanelProps) {
  const { visible, onToggle, layerDefs = [] } = props;
  const { imageDefs = [], cameraOn, onToggleCamera } = props;

  // 分组的展开/收起态：记录“已收起”的分组 id 集合，默认全部展开。
  const [collapsed, setCollapsed] = useState<Set<string>>(new Set());
  const toggleCollapse = (groupId: string) => {
    setCollapsed((prev) => {
      const next = new Set(prev);
      if (next.has(groupId)) next.delete(groupId);
      else next.add(groupId);
      return next;
    });
  };

  const groups = buildGroups(layerDefs);

  const toggleIds = (ids: string[]) => {
    const state = groupState(ids, visible);
    const next = state !== "all"; // 非全选 → 全开；全选 → 全关
    ids.forEach((id) => onToggle(id, next));
  };

  // 单/多图层叶子条目：单图层用普通勾选，多图层（如边界线）用组合勾选态。
  const renderLeaf = (item: LeafItem) => {
    const state = groupState(item.ids, visible);
    return (
      <li key={item.key}>
        <label className="layer-row layer-leaf-row">
          <input
            type="checkbox"
            checked={item.ids.length === 1 ? !!visible[item.ids[0]] : state === "all"}
            ref={(el) => {
              if (el) el.indeterminate = item.ids.length > 1 && state === "partial";
            }}
            onChange={(e) =>
              item.ids.length === 1
                ? onToggle(item.ids[0], e.target.checked)
                : toggleIds(item.ids)
            }
          />
          <span>{item.label}</span>
        </label>
      </li>
    );
  };

  const renderGroup = (bucket: GroupBucket) => {
    const allIds = bucket.leaves.flatMap((l) => l.ids);
    const state = groupState(allIds, visible);
    const isCollapsed = collapsed.has(bucket.id);
    return (
      <li key={bucket.id} className="layer-group">
        <div className="layer-row layer-group-row">
          <button
            type="button"
            className="layer-toggle"
            aria-label={isCollapsed ? "展开" : "收起"}
            onClick={() => toggleCollapse(bucket.id)}
          >
            {isCollapsed ? "▶" : "▼"}
          </button>
          <input
            type="checkbox"
            checked={state === "all"}
            ref={(el) => {
              if (el) el.indeterminate = state === "partial";
            }}
            onChange={() => toggleIds(allIds)}
          />
          <span
            className="group-label"
            onClick={() => toggleCollapse(bucket.id)}
            style={{ cursor: "pointer" }}
          >
            {bucket.label}
          </span>
        </div>
        {!isCollapsed && (
          <ul className="layer-children">{bucket.leaves.map(renderLeaf)}</ul>
        )}
      </li>
    );
  };

  // “图像”组：渲染相机图像通道复选框。勾选态来自 cameraOn（订阅集合），
  // 默认不勾选；onChange 调用 onToggleCamera 触发订阅/退订。
  const renderImageGroup = () => {
    if (imageDefs.length === 0) return null;
    const isCollapsed = collapsed.has("image");
    const onCount = imageDefs.filter((d) => cameraOn?.has(d.id)).length;
    const state: "all" | "none" | "partial" =
      onCount === 0 ? "none" : onCount === imageDefs.length ? "all" : "partial";
    const toggleAll = () => {
      const next = state !== "all";
      imageDefs.forEach((d) => onToggleCamera?.(d.id, next));
    };
    return (
      <li key="image" className="layer-group">
        <div className="layer-row layer-group-row">
          <button
            type="button"
            className="layer-toggle"
            aria-label={isCollapsed ? "展开" : "收起"}
            onClick={() => toggleCollapse("image")}
          >
            {isCollapsed ? "▶" : "▼"}
          </button>
          <input
            type="checkbox"
            checked={state === "all"}
            ref={(el) => {
              if (el) el.indeterminate = state === "partial";
            }}
            onChange={toggleAll}
          />
          <span
            className="group-label"
            onClick={() => toggleCollapse("image")}
            style={{ cursor: "pointer" }}
          >
            图像
          </span>
        </div>
        {!isCollapsed && (
          <ul className="layer-children">
            {imageDefs.map((d) => (
              <li key={d.id}>
                <label className="layer-row layer-leaf-row">
                  <input
                    type="checkbox"
                    checked={!!cameraOn?.has(d.id)}
                  onChange={(e) => onToggleCamera?.(d.id, e.target.checked)}
                  />
                  <span>{d.label || d.id}</span>
                </label>
              </li>
            ))}
          </ul>
        )}
      </li>
    );
  };

  return (
    <div className="layer-panel">
      <h3>图层</h3>
      <ul className="layer-tree layer-tree-scroll">
        {groups.map(renderGroup)}
        {renderImageGroup()}
      </ul>
    </div>
  );
}