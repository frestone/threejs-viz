// Tauri 桌面外壳适配层。
// 在浏览器（纯 Web）中 isTauri() 为 false，所有对话框调用被安全跳过；
// 在 Tauri 桌面壳中可拿到本地 .mcap 的真实绝对路径，用于“本地路径直传（免上传）”通道。

// Tauri v2 在 window 注入 __TAURI_INTERNALS__（v1 为 __TAURI__）。
export function isTauri(): boolean {
  if (typeof window === "undefined") return false;
  const w = window as unknown as Record<string, unknown>;
  return "__TAURI_INTERNALS__" in w || "__TAURI__" in w;
}

// 上次浏览目录持久化 key：下次打开对话框默认定位到该目录，减少反复导航。
const LAST_BROWSE_DIR_KEY = "threejs-viz.lastBrowseDir";

// 从绝对路径中截取目录部分（兼容 posix / windows 分隔符）。
function dirOf(path: string): string {
  const i = Math.max(path.lastIndexOf("/"), path.lastIndexOf("\\"));
  return i > 0 ? path.slice(0, i) : path;
}

// 打开原生文件对话框，返回选中的 .mcap 绝对路径；取消或非 Tauri 环境返回 null。
// 会记住上次成功选中文件所在目录，作为下次打开的 defaultPath。
export async function pickMcapPath(): Promise<string | null> {
  if (!isTauri()) return null;
  // 动态 import：纯 Web 构建不会因缺少插件而在加载期报错。
  const { open } = await import("@tauri-apps/plugin-dialog");
  let lastDir: string | undefined;
  try {
    lastDir = window.localStorage.getItem(LAST_BROWSE_DIR_KEY) ?? undefined;
  } catch {
    lastDir = undefined;
  }
  const selected = (await open({
    multiple: false,
    directory: false,
    filters: [{ name: "MCAP", extensions: ["mcap"] }],
    ...(lastDir ? { defaultPath: lastDir } : {}),
  })) as string | string[] | null;
  const picked =
    typeof selected === "string"
      ? selected
      : Array.isArray(selected) && selected.length > 0
        ? selected[0]
        : null;
  if (picked) {
    try {
      window.localStorage.setItem(LAST_BROWSE_DIR_KEY, dirOf(picked));
    } catch {
      // localStorage 不可用时忽略，不影响选择结果。
    }
  }
  return picked;
}