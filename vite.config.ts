import { defineConfig } from "vite";
import react from "@vitejs/plugin-react";

// Three.js 版本不再需要 COOP/COEP（无 WASM 线程/SharedArrayBuffer 依赖）。
// watch.usePolling：规避容器内 inotify ENOSPC。
// proxy /ws：开发期把 WebSocket 转发到本地厚后端（协议与后端保持不变）。
export default defineConfig({
  plugins: [react()],
  // Tauri 期望固定端口且失败即退出（不静默换端口）；清屏交给 Tauri CLI。
  clearScreen: false,
  server: {
    port: 5173,
    strictPort: true,
    watch: {
      usePolling: true,
      // 不监听 Rust 外壳目录，避免与 tauri dev 的 cargo 编译互相触发。
      // 同时排除 Bazel 输出目录（bazel-out / bazel-bin / bazel-* 都是仓库根下的
      // 符号链接）：rules_foreign_cc 编译 FFmpeg 时会在里面持续写文件，不排除会
      // 让 dev server 反复整页刷新（macOS 上实测刷屏）。
      ignored: ["**/src-tauri/**", "**/bazel-*/**"],
    },
    proxy: {
      "/ws": {
        target: "ws://127.0.0.1:8080",
        ws: true,
        changeOrigin: true,
      },
    },
  },
  build: {
    target: "es2022",
  },
});
