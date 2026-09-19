// -----------------------------------------------------------------------------
// viz_ffi:桌面(Tauri)进程内直连传输的 C ABI(见 ADR-08 双部署双路径)。
//
// 与 platform/web/server.cpp 的 WsTransport 并列,是 IFrameSink 的第二个实现形态:
//   * WsTransport  : 会话帧 -> 字节封包 -> WebSocket 下发(前后端分离部署)
//   * viz_ffi      : 会话帧 -> 字节封包 -> C 回调直接交给 Rust(桌面同进程直连)
//
// 契约一致性(关键):下行回调 msg 的字节布局与 WsTransport 完全相同——
//   帧类 : [type:u8][seq:u64 LE][Frame proto bytes]  (type 1 普通 / 9 预取)
//   JSON 类: [type:u8][UTF-8 JSON]                    (type 2~8)
// 因此前端 FfiTransport 收到 msg 后走与 WS 路径完全相同的 parseServerMessage,
// 单一 frame.proto 序列化契约,零协议漂移。
//
// 上行控制则逐字段映射到下面的 C 函数(与 SessionControl 9 方法一一对应)。
// 本头文件是纯 C 接口(extern "C"),供 Rust bindgen / 手写 FFI 声明消费。
// -----------------------------------------------------------------------------
#ifndef VIZ_PLATFORM_FFI_VIZ_FFI_H
#define VIZ_PLATFORM_FFI_VIZ_FFI_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// 不透明会话句柄。由 viz_ffi_session_create 创建,viz_ffi_session_destroy 释放。
typedef struct VizFfiSession VizFfiSession;

// 下行消息回调:每条完整字节封包(布局同 WsTransport)回调一次。
//   msg  : 封包首字节为消息类型,后续见文件头契约说明。
//   len  : 封包字节长度。
//   ctx  : 创建会话时登记的用户上下文(Rust 侧 Box 指针),原样回传。
// 约定:回调在会话内部线程(播放/预取线程)触发,实现方须自行保证线程安全,
//       且不得在回调内长时间阻塞(会拖慢发帧节流)。msg 仅在回调期间有效,
//       如需留存必须拷贝。
typedef void (*VizFfiMessageCallback)(const uint8_t* msg, size_t len, void* ctx);

// 创建一个离线播放会话(内部构造 OfflineSession + FfiSink)。
//   on_message : 下行封包回调,非空。
//   ctx        : 用户上下文,原样透传给 on_message。
// 返回会话句柄;失败返回 NULL。
VizFfiSession* viz_ffi_session_create(VizFfiMessageCallback on_message, void* ctx);

// 设置 Debug 模式（1 开 / 0 关）。仅 Debug 模式输出性能诊断（image_*.csv、
// 逐帧/每 50 帧日志、前端 perf 落盘），默认关闭以免影响播放性能。
// 宿主入口(Rust)解析 VIZ_DEBUG 环境变量或 --debug 参数后调用；不调用时
// C++ 侧默认读 VIZ_DEBUG 环境变量，均未设置则为关闭。
void viz_ffi_set_debug(int enabled);

// 销毁会话:内部 Close() 停播放/预取线程并 join,释放资源。销毁后句柄失效。
void viz_ffi_session_destroy(VizFfiSession* session);

// === SessionControl 9 方法的 C 映射(字符串参数为 UTF-8,以 '\0' 结尾)===

// 打开数据源。source 语义同 WS 路径:本地绝对路径 / S3 record 名 / bucket/key。
// 桌面场景通常由 Rust 侧先解析本地路径再传绝对路径。
void viz_ffi_open(VizFfiSession* session, const char* source);

// 跳转到指定媒体时间(秒),generation 为代次(与前端 seek 自增代次一致)。
void viz_ffi_seek(VizFfiSession* session, double time_sec, uint64_t generation);

// 暂停/继续播放。
void viz_ffi_set_paused(VizFfiSession* session, int paused);

// 设置倍速(1.0 为正常速度)。
void viz_ffi_set_speed(VizFfiSession* session, double speed);

// 启动全速预取(后台全量落盘,幂等)。
void viz_ffi_start_prefetch(VizFfiSession* session);

// 订阅/取消某相机图像通道(开启后发帧前按需解码 HEVC)。
void viz_ffi_set_image_subscription(VizFfiSession* session, const char* channel, int enabled);

// 订阅/取消某点云通道。
void viz_ffi_set_point_cloud_subscription(VizFfiSession* session, const char* channel, int enabled);

// 订阅/取消某 RawData 通道。
void viz_ffi_set_raw_data_subscription(VizFfiSession* session, const char* channel, int enabled);

// 上报当前播放位置(秒)。驱动大数据(图像/RawData)独立流前瞻预解码;generation 用于代次隔离。
void viz_ffi_set_playhead(VizFfiSession* session, double time_sec, uint64_t generation);

// 关闭会话(停播放/预取但不销毁句柄,可再次 Open)。
void viz_ffi_close(VizFfiSession* session);

// 前端消费完一帧后调用,回落在途字节水位以解除预取背压。
// bytes 必须等于该帧下行封包的字节数(发送时的 payloadBytes)。
void viz_ffi_ack_frame_bytes(VizFfiSession* session, uint64_t bytes);

// === 仅供契约测试(P1-9)使用,非生产 API ===
// 用内部 FfiSink 对给定错误消息封包并触发一次回调(JSON 类封包:[type=4][json])。
// 使测试可在不打开真实数据源的前提下验证下行封包字节布局与 WsTransport 一致。
void viz_ffi_test_emit_error(VizFfiSession* session, const char* message);

// 用内部 FfiSink 对给定 seq + 空 Frame 封包并触发一次回调(帧类封包:
// [type=1][seq:u64 LE][Frame proto bytes])。用于校验帧头字节布局。
void viz_ffi_test_emit_frame(VizFfiSession* session, uint64_t seq);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // VIZ_PLATFORM_FFI_VIZ_FFI_H
