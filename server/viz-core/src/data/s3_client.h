#pragma once
// -----------------------------------------------------------------------------
// S3Client：不依赖 AWS SDK 的最小 S3 只读客户端。
//
// 传输层用 standalone Asio + asio::ssl（HTTPS），签名用 OpenSSL 手写 AWS
// SigV4（HMAC-SHA256 / SHA256）。仅实现只读所需的两个能力：
//   - headObjectSize() : HEAD 取对象总字节数（供 RandomAccessReader::size）。
//   - getRange()       : GET + Range: bytes=first-last，按需拉取字节区间。
//
// path-style 寻址（endpoint/bucket/key），与京东云等 S3 兼容服务一致
//（xmonitor s3_client_provider 用 useVirtualAddressing=false）。
//
// 凭证/endpoint 经 S3Config 注入，从配置文件 / 环境变量装载（configFromJson
// 提供内置默认作兜底）；生产环境应经环境变量或替换配置覆盖内置默认凭证。
// -----------------------------------------------------------------------------
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "data/random_reader.h"

namespace viz::s3 {

// S3 连接配置。endpoint 形如 "s3-internal.cn-north-1.jdcloud-oss.com"（不带
// scheme，默认 https:443）。region 用于 SigV4 credential scope。
struct S3Config {
    std::string endpoint;    // 主机名（可含 :port）
    std::string region;      // 如 "cn-north-1"
    std::string accessKey;
    std::string secretKey;
    bool verifySSL = false;  // 与 xmonitor 一致，内网自签证书默认不校验
    // 元数据服务基址：GET metaApiUrl + recordName 返回含 bucketName/s3Path 的
    // clipsInfos（见 meta_client.h fetchS3InfoFromMeta）。
    std::string metaApiUrl;
    // MCAP 本地缓存目录：按文件名打开时优先在此查找（is_regular_file 命中即用
    // 本地 McapDataSource 读取），未命中再走 S3 流式 range 拉取。空表示无本地缓存。
    std::string cacheDir;
};

// 从环境变量装载 S3Config：
//   VIZ_S3_ENDPOINT / VIZ_S3_REGION / VIZ_S3_ACCESS_KEY / VIZ_S3_SECRET_KEY
//   VIZ_S3_VERIFY_SSL（"1" 开启校验，默认关闭）
// 缺失项保持默认（endpoint/region 有内置占位默认，凭证无默认）。
S3Config configFromEnv();

// 从配置 JSON 文件装载 S3Config，并叠加环境变量覆盖。
// 优先级（高 -> 低）：环境变量 VIZ_S3_* / VIZ_CACHE_DIR > JSON 文件字段 > 内置默认值。
// JSON schema（全部可选）：
//   { "endpoint": "...", "region": "...",
//     "accessKey": "...", "secretKey": "...", "verifySSL": false,
//     "cacheDir": "本地 MCAP 缓存目录，按文件名打开时优先查找" }
// path 指向的文件不存在或解析失败时，退回内置默认值（含默认凭证），再叠加
// 环境变量覆盖，保证缺省仍可工作。
S3Config configFromJson(const std::string& path);

// 最小同步 S3 只读客户端。线程安全性：每次调用建立独立连接，无共享可变状态，
// 可跨线程使用（各自新建 Asio io_context）。
class S3Client {
 public:
    explicit S3Client(S3Config config);

    // HEAD 对象，返回字节数；失败抛 std::runtime_error。
    uint64_t headObjectSize(const std::string& bucket,
                            const std::string& key);

    // GET 对象 [first, last] 闭区间字节（含 last）。返回读到的字节；
    // 失败抛 std::runtime_error。
    std::vector<uint8_t> getRange(const std::string& bucket,
                                  const std::string& key, uint64_t first,
                                  uint64_t last);

 private:
    // 执行一次 HTTPS 请求（method: "GET"/"HEAD"），rangeHeader 为空则不加 Range。
    // 返回 (status, headers 原文, body)。
    struct HttpResponse {
        int status = 0;
        std::string headers;
        std::vector<uint8_t> body;
    };
    HttpResponse request(const std::string& method, const std::string& bucket,
                         const std::string& key,
                         const std::string& rangeHeader);

    S3Config cfg_;
};

// S3RandomReader：把 S3 对象包装成 RandomAccessReader，供 mcap_reader 的
// Summary/ChunkIndex 快路径按 Range 拉取。size() 由 HEAD 缓存；read() 走
// GET Range。真流式：只拉取命中目标 topic 的 Chunk 字节，不落盘全量。
class S3RandomReader : public viz::mcap::RandomAccessReader {
 public:
    S3RandomReader(std::shared_ptr<S3Client> client, std::string bucket,
               std::string key);

    uint64_t size() const override;
    size_t read(uint64_t offset, uint8_t* out, size_t len) override;

 private:
    std::shared_ptr<S3Client> client_;
    std::string bucket_;
    std::string key_;
    mutable uint64_t size_ = 0;
    mutable bool sizeKnown_ = false;
};

}  // namespace viz::s3