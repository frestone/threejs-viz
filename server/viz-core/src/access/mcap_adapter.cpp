// -----------------------------------------------------------------------------
// McapAdapter 实现：接口规整层，全部复用现有 McapDataSource 解码/建索引/组帧。
// 本地 vs S3 差异只在 Open() 选择 buildIndex(path) 还是 buildIndex(reader,label)。
// -----------------------------------------------------------------------------
#include "access/mcap_adapter.h"

#include <algorithm>
#include <cstdlib>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>

#include "config/config_paths.h"     // viz::config::resolveConfigPath（cwd 鲁棒性）
#include "config/profile_registry.h"  // viz::config::ProfileRegistry（A3b）
#include "data/mcap_reader.h"    // viz::mcap::listTopics
#include "data/s3_client.h"      // viz::s3::S3Client / S3RandomReader / configFromJson
#include "data/meta_client.h"    // viz::meta::fetchS3InfoFromMeta

namespace viz::access {

namespace {
// 部署形态：VIZ_DEPLOY_MODE=web 时禁止打开本地路径（仅允许 S3）。
bool isWebDeploy() {
    const char* m = std::getenv("VIZ_DEPLOY_MODE");
    return m && std::string(m) == "web";
}

// 判定 source 是否为 S3 定位：以 "s3://" 前缀，或不是一个存在的本地 .mcap 文件
// 且不含路径分隔的裸 record 名 / 含 bucket/key 形态时，都交给 S3 分支处理。
bool looksLikeS3(const std::string& source) {
    if (source.rfind("s3://", 0) == 0) return true;
    std::error_code ec;
    // 存在的本地 .mcap 文件优先按本地处理。
    if (std::filesystem::is_regular_file(source, ec)) return false;
    return true;  // 其余交给 S3 分支（record 名 / bucket/key）。
}

// 解析 s3://bucket/key 或 bucket/key。返回是否成功拆出 bucket+key。
bool parseS3Uri(const std::string& uri, std::string* bucket, std::string* key) {
    std::string s = uri;
    if (s.rfind("s3://", 0) == 0) s = s.substr(5);
    const auto slash = s.find('/');
    if (slash == std::string::npos || slash == 0 || slash + 1 >= s.size())
        return false;
    *bucket = s.substr(0, slash);
    *key = s.substr(slash + 1);
    return true;
}
}  // namespace

// ---- decoder / scene 选择（从 server.cpp 等价搬入，A3b 将改委托 ProfileRegistry）----

std::string McapAdapter::detectDecoderPath(
    const std::unordered_set<std::string>& topics) const {
    static constexpr const char* kHighPrecisionDecoder = "configs/decoder.json";
    static constexpr const char* kLightMapDecoder = "configs/decoder_lightmap.json";
    static constexpr const char* kHighPrecisionProbeTopic =
        "/localization/global_location";
    static constexpr const char* kLightMapProbeTopic = "/map/lite_definition_map";
    // 1) 环境变量强制覆盖，优先级最高（便于调试/单包指定）。
    if (const char* env = std::getenv("VIZ_DECODER_CONFIG")) {
        if (env[0] != '\0') {
            std::cerr << "[decoder] VIZ_DECODER_CONFIG override: " << env << std::endl;
            return env;
        }
    }
    // 2) 配置化多数据 profile 注册表（A3b）：识别规则+decoder 映射全部来自
    //    configs/data_profiles.json；新增数据源只加配置不改代码。
    {
        static constexpr const char* kProfilesPath = "configs/data_profiles.json";
        const auto registry = viz::config::ProfileRegistry::fromFile(
            viz::config::resolveConfigPath(kProfilesPath));
        if (!registry.empty()) {
            const std::string decoder = registry.select(topics);
            if (!decoder.empty()) return viz::config::resolveConfigPath(decoder);
            std::cerr << "[decoder] data_profiles.json 无命中，退化内建规则"
                      << std::endl;
        }
    }
    // 3) 退化：无注册表/未命中时沿用现有高精/轻图两内建规则（保证现网不回归）。
    if (topics.count(kHighPrecisionProbeTopic)) {
        std::cerr << "[decoder] detected HighPrecision -> " << kHighPrecisionDecoder
                  << std::endl;
        return viz::config::resolveConfigPath(kHighPrecisionDecoder);
    }
    if (topics.count(kLightMapProbeTopic)) {
        std::cerr << "[decoder] detected LightMap -> " << kLightMapDecoder << std::endl;
        return viz::config::resolveConfigPath(kLightMapDecoder);
    }
    std::cerr << "[decoder] no probe topic matched -> fallback "
              << kHighPrecisionDecoder << std::endl;
    return viz::config::resolveConfigPath(kHighPrecisionDecoder);
}

static std::string loadTextFrom(const std::string& path) {
    // cwd 鲁棒性：相对配置路径允许从 exe 目录 / /usr/bin 回退解析，
    // 防止运行形态切换（项目根启动 / 桌面 FFI / deb）时静默退化内置默认值。
    const std::string resolved = viz::config::resolveConfigPath(path);
    std::error_code ec;
    if (resolved.empty() || !std::filesystem::is_regular_file(resolved, ec)) return {};
    std::ifstream in(resolved, std::ios::binary);
    if (!in) return {};
    return std::string((std::istreambuf_iterator<char>(in)),
                       std::istreambuf_iterator<char>());
}

viz::McapDataSource McapAdapter::makeSourceFrom(const std::string& decoderPath) const {
    const std::string text = loadTextFrom(decoderPath);
    if (text.empty()) {
        std::cerr << "[decoder] '" << decoderPath
                  << "' unreadable, using built-in ROVER defaults" << std::endl;
        return viz::McapDataSource{};
    }
    try {
        auto decoder = viz::McapDataSource::DecoderConfig::fromJson(text);
        viz::McapDataSource::Topics topics;
        try {
            topics = viz::McapDataSource::Topics::fromJson(text);
        } catch (const std::exception& te) {
            std::cerr << "[decoder] topics parse failed: " << te.what()
                      << " (using default topics)" << std::endl;
        }
        return viz::McapDataSource{std::move(topics), std::move(decoder)};
    } catch (const std::exception& e) {
        std::cerr << "[decoder] parse failed for " << decoderPath << ": " << e.what()
                  << " (falling back to built-in defaults)" << std::endl;
        return viz::McapDataSource{};
    }
}

viz::SceneConfig McapAdapter::loadSceneConfigFrom(const std::string& decoderPath) const {
    viz::SceneConfig scene = viz::SceneConfig::defaults();
    const std::string text = loadTextFrom(decoderPath);
    if (text.empty()) return scene;
    try {
        viz::SceneConfig::loadLayersFromJson(scene, text);
    } catch (const std::exception& e) {
        std::cerr << "[scene] layer parse failed: " << e.what() << std::endl;
    }
    try {
        viz::SceneConfig::loadChartsFromJson(scene, text);
    } catch (const std::exception& e) {
        std::cerr << "[scene] chart parse failed: " << e.what() << std::endl;
    }
    try {
        viz::SceneConfig::loadPointCloudsFromJson(scene, text);
    } catch (const std::exception& e) {
        std::cerr << "[scene] pointcloud parse failed: " << e.what() << std::endl;
    }
    try {
        viz::SceneConfig::loadImageChannelsFromJson(scene, text);
        viz::SceneConfig::loadRawDataFromJson(scene, text);
    } catch (const std::exception& e) {
        std::cerr << "[scene] image channel parse failed: " << e.what() << std::endl;
    }
    return scene;
}

// ---- Open ----

bool McapAdapter::Open(const std::string& source) {
    source_ = source;
    if (looksLikeS3(source)) {
        return openS3(source);
    }
    if (isWebDeploy()) {
        std::cerr << "[access] web deploy 拒绝本地路径: " << source << std::endl;
        return false;
    }
    return openLocal(source);
}

bool McapAdapter::openLocal(const std::string& path) {
    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec)) {
        std::cerr << "[access] 本地 MCAP 不存在: " << path << std::endl;
        return false;
    }
    // 保留一个长期打开的 reader，供订阅式图像按需随机读（图像不进 buildIndex）。
    auto reader = viz::mcap::openFileReader(path);
    if (!reader) {
        std::cerr << "[access] 本地 MCAP 无法打开: " << path << std::endl;
        return false;
    }
    std::unordered_set<std::string> topics;
    try {
        topics = viz::mcap::listTopics(*reader);
    } catch (const std::exception& e) {
        std::cerr << "[access] listTopics failed: " << e.what() << std::endl;
    }
    const std::string decoderPath = detectDecoderPath(topics);
    viz::McapDataSource src = makeSourceFrom(decoderPath);
    scene_ = loadSceneConfigFrom(decoderPath);
    auto index = src.buildIndex(*reader, path);
    if (!index || index->frameCount() == 0) {
        std::cerr << "[access] 本地 MCAP 未解码出任何 Frame: " << path << std::endl;
        return false;
    }
    index_ = std::move(index);
    reader_ = std::move(reader);
    return true;
}

bool McapAdapter::openS3(const std::string& source) {
    auto cfg = viz::s3::configFromJson(
        viz::config::resolveConfigPath("configs/s3.json"));
    std::string bucket, key;
    // 优先按 s3://bucket/key 或 bucket/key 直接解析；否则视为 record 名走元数据服务。
    if (!parseS3Uri(source, &bucket, &key)) {
        std::string err;
        if (!viz::meta::fetchS3InfoFromMeta(cfg.metaApiUrl, source, &bucket, &key,
                                            &err)) {
            std::cerr << "[access] 元数据查询失败(record=" << source << "): " << err
                      << std::endl;
            return false;
        }
    }
    if (cfg.endpoint.empty() || cfg.accessKey.empty() || cfg.secretKey.empty()) {
        std::cerr << "[access] S3 配置缺失(endpoint/accessKey/secretKey)" << std::endl;
        return false;
    }
    auto client = std::make_shared<viz::s3::S3Client>(cfg);
    auto reader = std::make_shared<viz::s3::S3RandomReader>(client, bucket, key);
    std::unordered_set<std::string> topics;
    try {
        topics = viz::mcap::listTopics(*reader);
    } catch (const std::exception& e) {
        std::cerr << "[access] listTopics(S3) failed: " << e.what() << std::endl;
    }
    const std::string decoderPath = detectDecoderPath(topics);
    viz::McapDataSource src = makeSourceFrom(decoderPath);
    scene_ = loadSceneConfigFrom(decoderPath);
    auto index = src.buildIndex(*reader, bucket + "/" + key);
    if (!index || index->frameCount() == 0) {
        std::cerr << "[access] S3 MCAP 未解码出任何 Frame: " << bucket << "/" << key
                  << std::endl;
        return false;
    }
    index_ = std::move(index);
    reader_ = std::move(reader);
    return true;
}

// ---- 读取接口 ----

DataSourceMeta McapAdapter::GetMeta() const {
    DataSourceMeta meta;
    meta.mode = AccessMode::kRandom;
    if (!index_) return meta;
    meta.frameCount = index_->frameCount();
    meta.durationSec = index_->duration();
    meta.frameDt = meta.frameCount > 1 ? meta.durationSec / (meta.frameCount - 1) : 0.0;
    meta.lightMapMode = index_->lightMapMode;
    return meta;
}

bool McapAdapter::ReadFrame(std::size_t i, Frame& out,
                            const SceneConfig* scene) const {
    if (!index_ || i >= index_->frameCount()) return false;
    try {
        out = viz::McapDataSource::assembleFrame(*index_, i, scene ? scene : &scene_);
        return true;
    } catch (const std::exception& e) {
        std::cerr << "[access] assembleFrame(" << i << ") failed: " << e.what()
                  << std::endl;
        return false;
    }
}

bool McapAdapter::ReadStaticMap(Frame& out) const {
    if (!index_) return false;
    try {
        out = viz::McapDataSource::assembleStaticMap(*index_);
        // 仅含地图 layers 时才算命中；无地图（如高精模式）返回 false 由会话层跳过。
        return out.layers_size() > 0;
    } catch (const std::exception& e) {
        std::cerr << "[access] assembleStaticMap failed: " << e.what()
                  << std::endl;
        return false;
    }
}

std::size_t McapAdapter::IndexAtTime(double timeSec) const {
    if (!index_ || index_->frameCount() == 0) return 0;
    return index_->indexAtTime(timeSec);
}

uint64_t McapAdapter::FrameLogTimeNs(std::size_t index) const {
    if (!index_ || index >= index_->frameCount()) return 0;
    // frameTime 返回相对起始秒；叠加 t0(首帧绝对纳秒) 得到该帧绝对 log_time 纳秒。
    return index_->t0 +
           static_cast<uint64_t>(std::llround(index_->frameTime(index) * 1e9));
}

bool McapAdapter::ReadImageMessage(const std::string& topic, uint64_t logTimeNs,
                                   std::vector<uint8_t>& out,
                                   uint64_t* outLogTimeNs) const {
    if (!reader_ || topic.empty()) return false;
    // 空窗口才倍增向前搜索，只保留单条候选，不收集历史序列。
    uint64_t bestTime = 0;
    bool found = false;
    std::vector<uint8_t> bestData;
    const auto readWindow = [&](uint64_t start, uint64_t end, bool latest) {
        viz::mcap::readMessages(
            *reader_,
            [&](const std::string&, uint64_t lt, const uint8_t* d, size_t n) {
                if (!found || (latest ? lt > bestTime : lt < bestTime)) {
                    found = true;
                    bestTime = lt;
                    bestData.assign(d, d + n);
                }
            },
            viz::mcap::TopicFilter{topic}, start, end);
    };
    try {
        uint64_t windowNs = 1000000000ULL;
        uint64_t end = logTimeNs;
        while (true) {
            const uint64_t start = logTimeNs > windowNs ? logTimeNs - windowNs : 0;
            readWindow(start, end, true);
            if (found || start == 0) break;
            end = start - 1;
            windowNs = windowNs > UINT64_MAX / 2 ? UINT64_MAX : windowNs * 2;
        }
        if (!found && logTimeNs != UINT64_MAX)
            readWindow(logTimeNs + 1, UINT64_MAX, false);
    } catch (const std::exception& e) {
        std::cerr << "[access] ReadImageMessage(" << topic << ") failed: " << e.what()
                  << std::endl;
        return false;
    }
    if (!found) return false;
    out = std::move(bestData);
    if (outLogTimeNs) *outLogTimeNs = bestTime;
    return true;
}

bool McapAdapter::ReadImageMessagesRange(const std::string& topic,
                                        uint64_t startTimeNs, uint64_t endTimeNs,
                                        std::vector<ImageMsg>& out) const {
    out.clear();
    if (!reader_ || topic.empty() || startTimeNs > endTimeNs) return false;
    try {
        viz::mcap::readMessages(
            *reader_,
            [&](const std::string&, uint64_t lt, const uint8_t* d, size_t n) {
                ImageMsg m;
                m.logTimeNs = lt;
                m.data.assign(d, d + n);
                out.push_back(std::move(m));
            },
            viz::mcap::TopicFilter{topic}, startTimeNs, endTimeNs);
    } catch (const std::exception& e) {
        std::cerr << "[access] ReadImageMessagesRange(" << topic << ") failed: " << e.what()
                  << std::endl;
        out.clear();
        return false;
    }
    std::sort(out.begin(), out.end(),
              [](const ImageMsg& a, const ImageMsg& b) { return a.logTimeNs < b.logTimeNs; });
    return !out.empty();
}

bool McapAdapter::ReadImageMessagesUpTo(const std::string& topic, uint64_t logTimeNs,
                                        std::vector<ImageMsg>& out) const {
    if (ReadImageMessagesRange(topic, 0, logTimeNs, out)) return true;
    // 首帧之前仍回退到第一条消息。
    ImageMsg first;
    if (!ReadImageMessage(topic, logTimeNs, first.data, &first.logTimeNs)) return false;
    out.push_back(std::move(first));
    return true;
}

void McapAdapter::Close() {
    index_.reset();
    reader_.reset();
    scene_ = SceneConfig{};
    source_.clear();
}

// ---- 工厂 ----

std::unique_ptr<IDataAccessAdapter> MakeAdapter(const std::string& /*source*/) {
    // 当前唯一实现为随机访问 MCAP 适配器；顺序流式源留待后续扩展。
    return std::make_unique<McapAdapter>();
}

}  // namespace viz::access// build-verify 1788336711
