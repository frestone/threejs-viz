// threejs-viz 自包含 C++ 后端 (零外部依赖)。
// 监听 127.0.0.1:8080,提供符合前端 FrameStream 协议的 WebSocket 推流:
//   msgType: FRAME=1 / STREAM_INFO=2 / FILE_LIST=3 / ERROR=4 / UPLOAD_STATUS=5 / CHART_DEFS=6
//   FRAME 帧 = [u8 type][u64 seq LE][protobuf Frame bytes]
// 数据源为合成的行驶场景(自车 + 障碍框 + 车道线 + 速度曲线),让前端跑通完整渲染。
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "frame_model.h"
#include "ws_server.h"

namespace {

constexpr uint8_t kFrame = 1, kStreamInfo = 2, kFileList = 3, kError = 4,
                  kUploadStatus = 5, kChartDefs = 6;

constexpr int kFrameCount = 600;      // 合成帧数
constexpr double kFrameDt = 0.05;     // 20 Hz
const std::string kFileName = "synthetic_demo.mcap";

void appendU64Le(std::string& out, uint64_t v) {
  for (int i = 0; i < 8; ++i) out.push_back(static_cast<char>((v >> (8 * i)) & 0xff));
}

// 极简 JSON 取值:从形如 "type":"open" / "timeSec":1.5 / "paused":true 提取。
std::string jsonStr(const std::string& j, const std::string& key) {
  const std::string pat = "\"" + key + "\"";
  size_t p = j.find(pat);
  if (p == std::string::npos) return "";
  p = j.find(':', p);
  if (p == std::string::npos) return "";
  ++p;
  while (p < j.size() && (j[p] == ' ' || j[p] == '\t')) ++p;
  if (p < j.size() && j[p] == '"') {
    size_t e = j.find('"', p + 1);
    return j.substr(p + 1, e - p - 1);
  }
  size_t e = j.find_first_of(",}", p);
  return j.substr(p, e - p);
}

double jsonNum(const std::string& j, const std::string& key, double def) {
  const std::string s = jsonStr(j, key);
  if (s.empty()) return def;
  try { return std::stod(s); } catch (...) { return def; }
}

// 合成第 idx 帧:自车沿 x 匀速前进带轻微转向,前方两辆动态障碍车,两侧车道线,速度曲线。
model::Frame buildFrame(int idx) {
  model::Frame f;
  const double t = idx * kFrameDt;
  f.t = t;

  const float egoX = static_cast<float>(t * 8.0);           // 8 m/s
  const float egoY = static_cast<float>(std::sin(t * 0.3) * 2.0);
  const float egoYaw = static_cast<float>(std::cos(t * 0.3) * 0.15);
  f.egoAnchor = {egoX, egoY, 0.0f};
  f.egoYaw = egoYaw;
  f.egoValid = true;

  // 障碍物图层 (BOX)
  {
    model::LayerData layer;
    layer.kind = model::BOX;
    for (int k = 0; k < 3; ++k) {
      model::GeometryItem g;
      g.hasPosition = true;
      g.position = {egoX + 15.0f + k * 12.0f + static_cast<float>(std::sin(t + k) * 3.0),
                    static_cast<float>((k - 1) * 3.5 + std::cos(t * 0.5 + k)),
                    0.0f};
      g.hasSize = true;
      g.size = {4.5f, 2.0f, 1.6f};
      g.heading = static_cast<float>(std::sin(t * 0.2 + k) * 0.2);
      g.id = k + 1;
      g.type = k % 3;
      g.score = 0.8f;
      layer.items.push_back(std::move(g));
    }
    f.layers.emplace_back("obstacles", std::move(layer));
  }

  // 车道线图层 (LINESTRIP) — 左右两条
  {
    model::LayerData layer;
    layer.kind = model::LINESTRIP;
    for (int side = -1; side <= 1; side += 2) {
      model::GeometryItem g;
      for (int s = 0; s < 40; ++s) {
        const float x = egoX - 10.0f + s * 2.0f;
        const float y = static_cast<float>(side * 4.0 + std::sin(x * 0.05) * 1.5);
        g.points.push_back({x, y, 0.0f});
      }
      layer.items.push_back(std::move(g));
    }
    f.layers.emplace_back("lanes", std::move(layer));
  }

  // 速度曲线图表 (最近 5 秒滑窗)
  {
    model::ChartData chart;
    chart.title = "Ego Speed";
    chart.xLabel = "t (s)";
    chart.yLabel = "v (m/s)";
    model::ChartSeries series;
    series.name = "speed";
    series.kind = model::CS_LINE;
    series.color = "#4ade80";
    const int window = 100;
    const int start = idx > window ? idx - window : 0;
    for (int i = start; i <= idx; ++i) {
      const double ti = i * kFrameDt;
      series.x.push_back(static_cast<float>(ti));
      series.y.push_back(static_cast<float>(8.0 + std::sin(ti * 0.5) * 2.0));
    }
    chart.series.push_back(std::move(series));
    f.charts.emplace_back("speed", std::move(chart));
  }

  return f;
}

// 每个 WebSocket 连接对应一个播放会话。
class Session {
 public:
  explicit Session(ws::Connection& conn) : conn_(conn) {}

  void run() {
    sendChartDefs();
    // 读线程处理控制指令,主循环推帧。
    worker_ = std::thread([this] { pushLoop(); });
    // 读循环 (阻塞)
    std::string payload;
    while (true) {
      int opcode = conn_.recvFrame(payload);
      if (opcode == 0 || opcode == 0x8) break;  // 关闭/错误
      if (opcode == 0x1) handleText(payload);   // 文本 = 控制指令
      // 二进制上行(上传分块)在合成后端忽略。
    }
    {
      std::lock_guard<std::mutex> lock(mu_);
      stopped_ = true;
      cv_.notify_all();
    }
    if (worker_.joinable()) worker_.join();
  }

 private:
  void sendJson(uint8_t type, const std::string& json) {
    std::string payload(1, static_cast<char>(type));
    payload += json;
    std::lock_guard<std::mutex> sendLock(sendMu_);
    conn_.sendBinary(payload);
  }

  void sendChartDefs() {
    sendJson(kChartDefs,
             "{\"charts\":[{\"id\":\"speed\",\"title\":\"Ego Speed\",\"visible\":true}]}");
  }

  void sendFileList() {
    sendJson(kFileList,
             "{\"files\":[{\"name\":\"" + kFileName + "\",\"sizeBytes\":1048576}]}");
  }

  void sendStreamInfo() {
    uint64_t gen;
    { std::lock_guard<std::mutex> lock(mu_); gen = generation_; }
    const double duration = (kFrameCount - 1) * kFrameDt;
    sendJson(kStreamInfo, "{\"durationSec\":" + std::to_string(duration) +
                              ",\"frameCount\":" + std::to_string(kFrameCount) +
                              ",\"generation\":" + std::to_string(gen) + "}");
  }

  void sendFrameMsg(const model::Frame& frame, uint64_t seq) {
    std::string payload(1, static_cast<char>(kFrame));
    appendU64Le(payload, seq);
    payload += model::serializeFrame(frame);
    std::lock_guard<std::mutex> sendLock(sendMu_);
    conn_.sendBinary(payload);
  }

  void handleText(const std::string& msg) {
    const std::string type = jsonStr(msg, "type");
    if (type == "listFiles") {
      sendFileList();
    } else if (type == "open") {
      open();
    } else if (type == "seek") {
      const double timeSec = jsonNum(msg, "timeSec", 0.0);
      std::lock_guard<std::mutex> lock(mu_);
      ++generation_;
      frameIdx_ = clampIdx(static_cast<int>(timeSec / kFrameDt));
      cv_.notify_all();
      lockSendInfoPending_ = true;
    } else if (type == "setPaused") {
      const std::string p = jsonStr(msg, "paused");
      std::lock_guard<std::mutex> lock(mu_);
      paused_ = (p == "true");
      cv_.notify_all();
    } else if (type == "setSpeed") {
      const double sp = jsonNum(msg, "speed", 1.0);
      std::lock_guard<std::mutex> lock(mu_);
      if (sp > 0 && sp <= 16) speed_ = sp;
    }
    // uploadBegin/uploadComplete/uploadCancel/setLayerVisible: 合成后端无需处理。
  }

  void open() {
    {
      std::lock_guard<std::mutex> lock(mu_);
      opened_ = true;
      frameIdx_ = 0;
      paused_ = false;
    }
    sendStreamInfo();
    { std::lock_guard<std::mutex> lock(mu_); cv_.notify_all(); }
  }

  static int clampIdx(int i) {
    if (i < 0) return 0;
    if (i >= kFrameCount) return kFrameCount - 1;
    return i;
  }

  void pushLoop() {
    using Clock = std::chrono::steady_clock;
    auto next = Clock::now();
    while (true) {
      int idx;
      uint64_t gen;
      double speed;
      bool sendInfo = false;
      {
        std::unique_lock<std::mutex> lock(mu_);
        cv_.wait(lock, [this] { return stopped_ || (opened_ && !paused_); });
        if (stopped_) return;
        idx = frameIdx_;
        gen = generation_;
        speed = speed_;
        if (lockSendInfoPending_) { sendInfo = true; lockSendInfoPending_ = false; }
        if (idx >= kFrameCount - 1) {
          paused_ = true;
          continue;
        }
        frameIdx_ = idx + 1;
      }
      if (sendInfo) sendStreamInfo();

      const model::Frame frame = buildFrame(idx);
      const uint64_t seq = (gen << 32) | static_cast<uint64_t>(idx);
      sendFrameMsg(frame, seq);

      next += std::chrono::duration_cast<Clock::duration>(
          std::chrono::duration<double>(kFrameDt / speed));
      std::this_thread::sleep_until(next);
      if (Clock::now() - next > std::chrono::seconds(1)) next = Clock::now();
    }
  }

  ws::Connection& conn_;
  std::thread worker_;
  std::mutex mu_;
  std::mutex sendMu_;
  std::condition_variable cv_;
  bool stopped_ = false;
  bool opened_ = false;
  bool paused_ = true;
  bool lockSendInfoPending_ = false;
  int frameIdx_ = 0;
  uint64_t generation_ = 0;
  double speed_ = 1.0;
};

}  // namespace

int main(int argc, char** argv) {
  int port = 8080;
  if (argc > 1) {
    try { port = std::stoi(argv[1]); } catch (...) {}
  }
  ws::Server server;
  if (!server.listen(port)) {
    std::cerr << "无法监听端口 " << port << "\n";
    return 1;
  }
  std::cout << "threejs-viz C++ 后端已启动: ws://127.0.0.1:" << port << "/ws\n";
  std::cout << "合成场景: " << kFrameCount << " 帧 @ " << (1.0 / kFrameDt) << " Hz\n";
  server.run([](ws::Connection& conn) {
    try {
      Session session(conn);
      session.run();
    } catch (const std::exception& e) {
      std::cerr << "会话异常: " << e.what() << "\n";
    }
  });
  return 0;
}