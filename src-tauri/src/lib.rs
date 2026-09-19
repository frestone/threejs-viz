// Three.js Viz 桌面外壳（Tauri v2）。
// 桌面数据链路通过静态链接的 viz_ffi 在当前进程内运行，不再启动 sidecar/WebSocket。

use std::sync::{Mutex, MutexGuard};

use tauri::ipc::Channel;
use tauri::State;

mod ffi;

// 单个桌面窗口共享一个离线播放会话。Mutex 只保护句柄的创建/销毁和控制调用；
// C++ 下行回调不获取该锁，只复制消息并投递 Tauri event，避免回调重入死锁。
struct FfiState(Mutex<Option<ffi::VizSession>>);

fn lock_session<'a>(
    state: &'a State<'_, FfiState>,
) -> Result<MutexGuard<'a, Option<ffi::VizSession>>, String> {
    state
        .0
        .lock()
        .map_err(|_| "FFI 会话状态锁已损坏".to_string())
}

// Debug 模式解析：VIZ_DEBUG 环境变量（1/true/on/yes，大小写不敏感）或
// 命令行 --debug 任一开启即视为 Debug 模式。打包/生产默认关闭。
fn env_truthy(value: &str) -> bool {
    matches!(
        value.trim().to_ascii_lowercase().as_str(),
        "1" | "true" | "on" | "yes"
    )
}

fn debug_enabled() -> bool {
    // debug 构建（tauri dev / tauri build --debug）默认开启，便于开发期排查；
    // release 打包默认关闭，仅显式 VIZ_DEBUG=1 或 --debug 才开启。
    cfg!(debug_assertions)
        || std::env::var("VIZ_DEBUG").map(|v| env_truthy(&v)).unwrap_or(false)
        || std::env::args().any(|arg| arg == "--debug")
}

fn with_session(
    state: &State<'_, FfiState>,
    action: impl FnOnce(&ffi::VizSession),
) -> Result<(), String> {
    let guard = lock_session(state)?;
    let session = guard
        .as_ref()
        .ok_or_else(|| "FFI 会话尚未连接".to_string())?;
    action(session);
    Ok(())
}

#[tauri::command]
fn ffi_connect(
    channel: Channel<Vec<u8>>,
    state: State<'_, FfiState>,
) -> Result<(), String> {
    let mut guard = lock_session(&state)?;
    if guard.is_some() {
        return Ok(());
    }

    // 下行大帧（~830KB/帧）经 Tauri Channel 二进制快速通道投递，绝不用 emit —— emit 会把
    // Vec<u8> 序列化成 JSON 整数数组（830KB→数 MB 文本），压垮 WebView 主线程导致卡死。
    // Channel 传 Vec<u8> 走二进制路径零 JSON 开销，前端 onmessage 直接收到 ArrayBuffer。
    let session = ffi::VizSession::create(Box::new(move |bytes| {
        // C++ 回调内的消息缓冲仅在本次调用期间有效，必须先复制后再跨线程投递。
        if let Err(error) = channel.send(bytes.to_vec()) {
            eprintln!("[viz_ffi] 投递下行消息失败: {error}");
        }
    }))
    .ok_or_else(|| "创建 FFI 会话失败".to_string())?;

    *guard = Some(session);
    Ok(())
}

#[tauri::command]
fn ffi_open(source: String, state: State<'_, FfiState>) -> Result<(), String> {
    with_session(&state, |session| session.open(&source))
}

#[tauri::command]
fn ffi_seek(time_sec: f64, generation: u64, state: State<'_, FfiState>) -> Result<(), String> {
    with_session(&state, |session| session.seek(time_sec, generation))
}

#[tauri::command]
fn ffi_set_paused(paused: bool, state: State<'_, FfiState>) -> Result<(), String> {
    with_session(&state, |session| session.set_paused(paused))
}

#[tauri::command]
fn ffi_set_speed(speed: f64, state: State<'_, FfiState>) -> Result<(), String> {
    with_session(&state, |session| session.set_speed(speed))
}

#[tauri::command]
fn ffi_start_prefetch(state: State<'_, FfiState>) -> Result<(), String> {
    with_session(&state, |session| session.start_prefetch())
}

#[tauri::command]
fn ffi_ack_frame(bytes: u64, state: State<'_, FfiState>) -> Result<(), String> {
    with_session(&state, |session| session.ack_frame_bytes(bytes))
}

#[tauri::command]
fn ffi_subscribe_image(
    channel: String,
    enabled: bool,
    state: State<'_, FfiState>,
) -> Result<(), String> {
    with_session(&state, |session| {
        session.set_image_subscription(&channel, enabled)
    })
}

#[tauri::command]
fn ffi_subscribe_point_cloud(
    channel: String,
    enabled: bool,
    state: State<'_, FfiState>,
) -> Result<(), String> {
    with_session(&state, |session| {
        session.set_point_cloud_subscription(&channel, enabled)
    })
}

#[tauri::command]
fn ffi_subscribe_raw_data(
    channel: String,
    enabled: bool,
    state: State<'_, FfiState>,
) -> Result<(), String> {
    with_session(&state, |session| {
        session.set_raw_data_subscription(&channel, enabled)
    })
}

#[tauri::command]
fn ffi_set_playhead(
    time_sec: f64,
    generation: u64,
    state: State<'_, FfiState>,
) -> Result<(), String> {
    with_session(&state, |session| session.set_playhead(time_sec, generation))
}

#[tauri::command]
fn ffi_close(state: State<'_, FfiState>) -> Result<(), String> {
    // take 后在锁外 Drop：销毁会话会等待 C++ 播放/预取线程退出，避免长时间持锁。
    let session = lock_session(&state)?.take();
    drop(session);
    Ok(())
}

// 前端性能诊断日志转发到进程 stderr，与后端 [viz_ffi] 日志汇聚到同一处（终端/nohup.out）。
#[tauri::command]
fn ffi_log(line: String) {
    if !debug_enabled() {
        return; // 仅 Debug 模式转发，避免非调试场景的 IPC/输出开销
    }
    eprintln!("[frontend] {line}");
}

// 前端查询 Debug 模式状态：开启时才允许 perf 落盘与诊断日志。
#[tauri::command]
fn ffi_is_debug() -> bool {
    debug_enabled()
}

// 前端图像各阶段耗时批量落盘（到达→Blob→<img> onLoad）。
// 目录与后端一致：VIZ_PERF_LOG 指定，默认 $HOME/threejs-viz-perf。
// 返回文件路径，前端仅首次打印一次，便于定位产物。
fn perf_log_dir() -> std::path::PathBuf {
    if let Ok(dir) = std::env::var("VIZ_PERF_LOG") {
        if !dir.is_empty() {
            return std::path::PathBuf::from(dir);
        }
    }
    #[cfg(windows)]
    if let Ok(profile) = std::env::var("USERPROFILE") {
        if !profile.is_empty() {
            return std::path::PathBuf::from(profile).join("threejs-viz-perf");
        }
    }
    #[cfg(not(windows))]
    if let Ok(home) = std::env::var("HOME") {
        if !home.is_empty() {
            return std::path::PathBuf::from(home).join("threejs-viz-perf");
        }
    }
    std::env::temp_dir().join("threejs-viz-perf")
}

#[tauri::command]
fn ffi_perf_log(batch: String) -> Result<String, String> {
    if !debug_enabled() {
        return Ok(String::new()); // 非 Debug 模式不落盘
    }
    use std::io::Write;
    let dir = perf_log_dir();
    std::fs::create_dir_all(&dir).map_err(|e| e.to_string())?;
    let path = dir.join("frontend_image.csv");
    let mut file = std::fs::OpenOptions::new()
        .create(true)
        .append(true)
        .open(&path)
        .map_err(|e| e.to_string())?;
    file.write_all(batch.as_bytes()).map_err(|e| e.to_string())?;
    file.write_all(b"\n").map_err(|e| e.to_string())?;
    Ok(path.to_string_lossy().to_string())
}

#[cfg_attr(mobile, tauri::mobile_entry_point)]
pub fn run() {
    // 保持 C ABI 符号从实际运行路径可达，避免 release 链接时被 --gc-sections 回收。
    ffi::ensure_linked();
    // Debug 模式（VIZ_DEBUG / --debug）：仅开启时后端输出性能诊断，默认关闭。
    ffi::VizSession::set_debug(debug_enabled());
    tauri::Builder::default()
        .plugin(tauri_plugin_dialog::init())
        .manage(FfiState(Mutex::new(None)))
        .invoke_handler(tauri::generate_handler![
            ffi_connect,
            ffi_open,
            ffi_seek,
            ffi_set_paused,
            ffi_set_speed,
            ffi_start_prefetch,
            ffi_ack_frame,
            ffi_subscribe_image,
            ffi_subscribe_point_cloud,
            ffi_subscribe_raw_data,
            ffi_set_playhead,
            ffi_close,
            ffi_log,
            ffi_is_debug,
            ffi_perf_log,
        ])
        .build(tauri::generate_context!())
        .expect("error while building threejs-viz desktop shell")
        .run(|_app_handle, _event| {});
}
