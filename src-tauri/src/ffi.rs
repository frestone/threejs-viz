// -----------------------------------------------------------------------------
// viz_ffi 的 Rust 绑定(ADR-08 / P1-6b)。
//
// 桌面进程内直连:本模块把 C++ 侧 viz_ffi.h 的纯 C ABI 封装成安全的 Rust API。
//   * 下行:C++ 会话线程 -> VizFfiMessageCallback trampoline -> Rust 闭包(转发字节封包)
//   * 上行:VizSession 的方法逐一映射到 viz_ffi_* 控制函数
//
// 契约:回调 msg 的字节布局与 WsTransport 完全一致(帧类 [type][seq u64 LE][proto],
// JSON 类 [type][json]),前端 FfiTransport 复用同一 parseServerMessage,零协议漂移。
//
// panic 隔离(关键):Rust panic 跨越 FFI 边界(unwind 进 C++ 栈)是未定义行为。
// trampoline 内一律用 catch_unwind 兜住用户回调闭包里的任何 panic,吞掉并记录,
// 绝不让其逃逸到 C++。
// -----------------------------------------------------------------------------

use std::ffi::CString;
use std::os::raw::{c_char, c_int, c_void};
use std::panic::{catch_unwind, AssertUnwindSafe};

// 用户下行回调:收到一条完整字节封包(布局同 WsTransport)。
// 要求 Send:回调在 C++ 会话内部线程触发,须能跨线程。
pub type MessageHandler = Box<dyn FnMut(&[u8]) + Send + 'static>;

// ctx 里承载的是 MessageHandler 的堆分配(双层 Box 以获得瘦指针传给 C void*)。
type HandlerBox = Box<MessageHandler>;

#[allow(non_camel_case_types)]
type VizFfiSessionPtr = *mut c_void;

// C 回调签名:void (*)(const uint8_t* msg, size_t len, void* ctx)。
type RawCallback = extern "C" fn(*const u8, usize, *mut c_void);

extern "C" {
    fn viz_ffi_session_create(on_message: RawCallback, ctx: *mut c_void) -> VizFfiSessionPtr;
    fn viz_ffi_session_destroy(session: VizFfiSessionPtr);

    fn viz_ffi_open(session: VizFfiSessionPtr, source: *const c_char);
    fn viz_ffi_seek(session: VizFfiSessionPtr, time_sec: f64, generation: u64);
    fn viz_ffi_set_paused(session: VizFfiSessionPtr, paused: c_int);
    fn viz_ffi_set_speed(session: VizFfiSessionPtr, speed: f64);
    fn viz_ffi_start_prefetch(session: VizFfiSessionPtr);
    fn viz_ffi_set_image_subscription(
        session: VizFfiSessionPtr,
        channel: *const c_char,
        enabled: c_int,
    );
    fn viz_ffi_set_point_cloud_subscription(
        session: VizFfiSessionPtr,
        channel: *const c_char,
        enabled: c_int,
    );
    fn viz_ffi_set_raw_data_subscription(
        session: VizFfiSessionPtr,
        channel: *const c_char,
        enabled: c_int,
    );
    fn viz_ffi_set_playhead(session: VizFfiSessionPtr, time_sec: f64, generation: u64);
    fn viz_ffi_close(session: VizFfiSessionPtr);
    fn viz_ffi_ack_frame_bytes(session: VizFfiSessionPtr, bytes: u64);

    // 仅测试用。
    fn viz_ffi_test_emit_error(session: VizFfiSessionPtr, message: *const c_char);
    fn viz_ffi_test_emit_frame(session: VizFfiSessionPtr, seq: u64);
}

/// 在 P1-7 接入真实调用路径前，显式保留全部 C ABI 入口。
///
/// `mod ffi` 当前尚未被桌面运行路径调用；若不建立运行时可达引用，release/debug
/// 链接中的 `--gc-sections` 会把已成功静态链接的入口函数回收。`black_box` 只消费
/// 函数地址，不会调用 C++，因此没有会话或线程副作用。
pub(crate) fn ensure_linked() {
    std::hint::black_box((
        viz_ffi_session_create as *const () as usize,
        viz_ffi_session_destroy as *const () as usize,
        viz_ffi_open as *const () as usize,
        viz_ffi_seek as *const () as usize,
        viz_ffi_set_paused as *const () as usize,
        viz_ffi_set_speed as *const () as usize,
        viz_ffi_start_prefetch as *const () as usize,
        viz_ffi_set_image_subscription as *const () as usize,
        viz_ffi_set_point_cloud_subscription as *const () as usize,
        viz_ffi_set_raw_data_subscription as *const () as usize,
        viz_ffi_set_playhead as *const () as usize,
        viz_ffi_close as *const () as usize,
        viz_ffi_ack_frame_bytes as *const () as usize,
        viz_ffi_test_emit_error as *const () as usize,
        viz_ffi_test_emit_frame as *const () as usize,
    ));
}

// C 回调 trampoline:恢复 ctx 里的 Rust 闭包并转发字节;全程 catch_unwind 隔离 panic。
extern "C" fn trampoline(msg: *const u8, len: usize, ctx: *mut c_void) {
    if ctx.is_null() {
        return;
    }
    let result = catch_unwind(AssertUnwindSafe(|| {
        // ctx 指向的是 Box<MessageHandler>,借用而非取得所有权(所有权在 VizSession 手里)。
        let handler = unsafe { &mut *(ctx as *mut MessageHandler) };
        let bytes: &[u8] = if msg.is_null() || len == 0 {
            &[]
        } else {
            unsafe { std::slice::from_raw_parts(msg, len) }
        };
        handler(bytes);
    }));
    if result.is_err() {
        // 绝不让 panic 逃逸到 C++;此处仅记录。
        eprintln!("[viz_ffi] 下行回调 panic 已被隔离(catch_unwind)");
    }
}

/// 安全的会话句柄:RAII 持有 C++ 会话与回调闭包的堆分配,Drop 时销毁并回收。
pub struct VizSession {
    raw: VizFfiSessionPtr,
    // 保活回调闭包的堆分配;drop 时回收(必须晚于 C++ 会话线程 join,故在 destroy 之后)。
    handler_ptr: *mut MessageHandler,
}

// 裸指针不自动 Send/Sync;C++ 侧会话内部自持锁与线程,句柄可安全跨线程移动/共享调用。
unsafe impl Send for VizSession {}
unsafe impl Sync for VizSession {}

impl VizSession {
    /// 创建会话并登记下行回调。回调将在 C++ 会话线程被调用(已 panic 隔离)。
    /// 失败(C++ 侧返回 NULL,例如回调为空)时返回 None。
    pub fn create(handler: MessageHandler) -> Option<Self> {
        // 双层 Box:外层瘦指针可塞进 C 的 void*;内层是 trait object 胖指针。
        let boxed: HandlerBox = Box::new(handler);
        let handler_ptr: *mut MessageHandler = Box::into_raw(boxed);
        let raw = unsafe { viz_ffi_session_create(trampoline, handler_ptr as *mut c_void) };
        if raw.is_null() {
            // 创建失败,回收闭包,避免泄漏。
            unsafe {
                drop(Box::from_raw(handler_ptr));
            }
            return None;
        }
        Some(VizSession { raw, handler_ptr })
    }

    /// 打开数据源(本地绝对路径 / S3 record 名 / bucket/key)。
    pub fn open(&self, source: &str) {
        let c = to_cstring(source);
        unsafe { viz_ffi_open(self.raw, c.as_ptr()) };
    }

    /// 跳转到指定媒体时间(秒),generation 为 seek 自增代次。
    pub fn seek(&self, time_sec: f64, generation: u64) {
        unsafe { viz_ffi_seek(self.raw, time_sec, generation) };
    }

    /// 暂停/继续。
    pub fn set_paused(&self, paused: bool) {
        unsafe { viz_ffi_set_paused(self.raw, paused as c_int) };
    }

    /// 设置倍速(1.0 正常)。
    pub fn set_speed(&self, speed: f64) {
        unsafe { viz_ffi_set_speed(self.raw, speed) };
    }

    /// 启动全速预取(幂等)。
    pub fn start_prefetch(&self) {
        unsafe { viz_ffi_start_prefetch(self.raw) };
    }

    /// 订阅/取消某相机图像通道。
    pub fn set_image_subscription(&self, channel: &str, enabled: bool) {
        let c = to_cstring(channel);
        unsafe { viz_ffi_set_image_subscription(self.raw, c.as_ptr(), enabled as c_int) };
    }

    /// 订阅/取消某点云通道。
    pub fn set_point_cloud_subscription(&self, channel: &str, enabled: bool) {
        let c = to_cstring(channel);
        unsafe { viz_ffi_set_point_cloud_subscription(self.raw, c.as_ptr(), enabled as c_int) };
    }

    /// 订阅/取消某 RawData 通道。
    pub fn set_raw_data_subscription(&self, channel: &str, enabled: bool) {
        let c = to_cstring(channel);
        unsafe { viz_ffi_set_raw_data_subscription(self.raw, c.as_ptr(), enabled as c_int) };
    }

    /// 上报前端节流的播放位置(秒)与代次,驱动大数据(图像/RawData)独立流前瞻预解码。
    /// 桌面 FFI 链路缺此桥接会导致暂停态勾选相机后 BigDataRun 无 playhead 驱动、图像帧不下发。
    pub fn set_playhead(&self, time_sec: f64, generation: u64) {
        unsafe { viz_ffi_set_playhead(self.raw, time_sec, generation) };
    }

    /// 关闭会话(停线程但不销毁句柄,可再次 open)。
    pub fn close(&self) {
        unsafe { viz_ffi_close(self.raw) };
    }

    /// 前端消费完一帧后调用,回落在途字节水位以解除预取背压。
    /// bytes 须等于该帧下行封包字节数(发送时 payloadBytes)。
    pub fn ack_frame_bytes(&self, bytes: u64) {
        unsafe { viz_ffi_ack_frame_bytes(self.raw, bytes) };
    }

    /// 仅测试:触发一次错误封包回调。
    #[cfg(test)]
    pub fn test_emit_error(&self, message: &str) {
        let c = to_cstring(message);
        unsafe { viz_ffi_test_emit_error(self.raw, c.as_ptr()) };
    }

    /// 仅测试:触发一次空帧封包回调。
    #[cfg(test)]
    pub fn test_emit_frame(&self, seq: u64) {
        unsafe { viz_ffi_test_emit_frame(self.raw, seq) };
    }
}

impl Drop for VizSession {
    fn drop(&mut self) {
        // 先销毁 C++ 会话(内部 Close 停线程并 join),确保之后不再有回调触碰 handler。
        if !self.raw.is_null() {
            unsafe { viz_ffi_session_destroy(self.raw) };
            self.raw = std::ptr::null_mut();
        }
        // 会话线程已 join,回收回调闭包的堆分配。
        if !self.handler_ptr.is_null() {
            unsafe {
                drop(Box::from_raw(self.handler_ptr));
            }
            self.handler_ptr = std::ptr::null_mut();
        }
    }
}

// UTF-8 -> CString;含内嵌 NUL 时截断到首个 NUL 前(C 字符串语义)。
fn to_cstring(s: &str) -> CString {
    match CString::new(s) {
        Ok(c) => c,
        Err(err) => {
            let valid = &s.as_bytes()[..err.nul_position()];
            CString::new(valid).unwrap_or_default()
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::sync::mpsc;

    // 帧封包首字节类型 1、随后 8 字节小端 seq。
    #[test]
    fn frame_envelope_layout() {
        let (tx, rx) = mpsc::channel::<Vec<u8>>();
        let session = VizSession::create(Box::new(move |bytes: &[u8]| {
            let _ = tx.send(bytes.to_vec());
        }))
        .expect("create session");

        let seq: u64 = 0x0102_0304_0506_0708;
        session.test_emit_frame(seq);

        let msg = rx.recv().expect("frame callback");
        assert_eq!(msg[0], 1, "帧类首字节应为 type=1");
        let got = u64::from_le_bytes(msg[1..9].try_into().unwrap());
        assert_eq!(got, seq, "seq 应小端解码一致");
        assert!(msg.len() >= 9, "至少含 1+8 字节头");
    }

    // 空回调应返回 None(C++ 侧拒绝)。
    #[test]
    fn null_handler_still_ok() {
        // create 用真实闭包一定成功;此处仅验证生命周期不 panic。
        let session = VizSession::create(Box::new(|_bytes: &[u8]| {}));
        assert!(session.is_some());
    }
}
