// S3 只读客户端实现：asio::ssl HTTPS + OpenSSL 手写 AWS SigV4。见 s3_client.h。
#include "data/s3_client.h"

#include <openssl/hmac.h>
#include <openssl/sha.h>

#include <array>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <memory>
#include <fstream>
#include <sstream>
#include <stdexcept>

#include <nlohmann/json.hpp>

// standalone Asio（非 Boost）。ASIO_STANDALONE 由 BUILD defines 传入。
#include <asio.hpp>
#include <asio/ssl.hpp>

namespace viz::s3 {
namespace {

// ---- 十六进制 / 编码工具 -------------------------------------------------

std::string toHex(const unsigned char* data, size_t len) {
    static const char* kHex = "0123456789abcdef";
    std::string out;
    out.resize(len * 2);
    for (size_t i = 0; i < len; ++i) {
        out[2 * i] = kHex[data[i] >> 4];
        out[2 * i + 1] = kHex[data[i] & 0xF];
    }
    return out;
}

// SHA256 -> 32 字节。
std::array<unsigned char, SHA256_DIGEST_LENGTH> sha256(const std::string& in) {
    std::array<unsigned char, SHA256_DIGEST_LENGTH> out{};
    SHA256(reinterpret_cast<const unsigned char*>(in.data()), in.size(),
           out.data());
    return out;
}

std::string sha256Hex(const std::string& in) {
    auto d = sha256(in);
    return toHex(d.data(), d.size());
}

// HMAC-SHA256(key, data) -> raw bytes。
std::string hmacSha256(const std::string& key, const std::string& data) {
    unsigned char out[EVP_MAX_MD_SIZE];
    unsigned int outLen = 0;
    HMAC(EVP_sha256(), key.data(), static_cast<int>(key.size()),
         reinterpret_cast<const unsigned char*>(data.data()), data.size(), out,
         &outLen);
    return std::string(reinterpret_cast<char*>(out), outLen);
}

// RFC3986 URI 编码。encodeSlash=false 时保留 '/'（用于 canonical URI 的 key）。
std::string uriEncode(const std::string& s, bool encodeSlash) {
    std::string out;
    out.reserve(s.size() * 3);
    for (unsigned char c : s) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' ||
            c == '~') {
            out.push_back(static_cast<char>(c));
        } else if (c == '/' && !encodeSlash) {
            out.push_back('/');
        } else {
            char buf[4];
            std::snprintf(buf, sizeof(buf), "%%%02X", c);
            out += buf;
        }
    }
    return out;
}

// UTC 时间：amzDate=YYYYMMDDTHHMMSSZ，dateStamp=YYYYMMDD。
struct AmzTime {
    std::string amzDate;
    std::string dateStamp;
};
AmzTime nowAmzTime() {
    std::time_t t = std::time(nullptr);
    std::tm gm{};
#if defined(_WIN32)
    gmtime_s(&gm, &t);
#else
    gmtime_r(&t, &gm);
#endif
    char amz[32];
    char day[16];
    std::strftime(amz, sizeof(amz), "%Y%m%dT%H%M%SZ", &gm);
    std::strftime(day, sizeof(day), "%Y%m%d", &gm);
    return {amz, day};
}

// 从 endpoint 拆出 host 与 port（默认 443）。
void splitHostPort(const std::string& endpoint, std::string& host,
                   std::string& port) {
    auto pos = endpoint.find(':');
    if (pos == std::string::npos) {
        host = endpoint;
        port = "443";
    } else {
        host = endpoint.substr(0, pos);
        port = endpoint.substr(pos + 1);
    }
}

// 解析 HTTP 响应头里的 Content-Length（大小写不敏感）。找不到返回 0。
uint64_t parseContentLength(const std::string& headers) {
    std::string lower;
    lower.resize(headers.size());
    for (size_t i = 0; i < headers.size(); ++i)
        lower[i] = static_cast<char>(std::tolower(headers[i]));
    auto pos = lower.find("content-length:");
    if (pos == std::string::npos) return 0;
    pos += std::strlen("content-length:");
    while (pos < headers.size() && (headers[pos] == ' ')) ++pos;
    uint64_t v = 0;
    while (pos < headers.size() && headers[pos] >= '0' && headers[pos] <= '9') {
        v = v * 10 + static_cast<uint64_t>(headers[pos] - '0');
        ++pos;
    }
    return v;
}

}  // namespace

// ---- 环境变量配置 --------------------------------------------------------

namespace {
// 内置默认凭证：仅作为缺省兜底，便于开箱即用；生产环境应经 JSON 配置或
// 环境变量覆盖，避免依赖库内明文凭证。
constexpr const char* kDefaultAccessKey = "85C76AB2AB89077E8500CB6021A81EC7";
constexpr const char* kDefaultSecretKey = "EEDD3436DF9B9ED075A39DEE468F66A2";
// 元数据服务默认基址（与 xmonitor 一致）。GET metaApiUrl + recordName。
constexpr const char* kDefaultMetaApiUrl =
    "http://atumu-meta.jd.com/argo/generate_clips_info?recordName=";

// 用环境变量覆盖已有配置（仅当对应环境变量存在时才覆盖）。
void applyEnvOverride(S3Config& c) {
    if (const char* v = std::getenv("VIZ_S3_ENDPOINT")) c.endpoint = v;
    if (const char* v = std::getenv("VIZ_S3_REGION")) c.region = v;
    if (const char* v = std::getenv("VIZ_S3_ACCESS_KEY")) c.accessKey = v;
    if (const char* v = std::getenv("VIZ_S3_SECRET_KEY")) c.secretKey = v;
    if (const char* v = std::getenv("VIZ_S3_VERIFY_SSL"))
        c.verifySSL = std::string(v) == "1";
    if (const char* v = std::getenv("VIZ_S3_META_API_URL")) c.metaApiUrl = v;
    if (const char* v = std::getenv("VIZ_CACHE_DIR")) c.cacheDir = v;
}
}  // namespace

S3Config configFromEnv() {
    S3Config c;
    auto env = [](const char* k, const char* def) -> std::string {
        const char* v = std::getenv(k);
        return v ? std::string(v) : std::string(def);
    };
    c.endpoint = env("VIZ_S3_ENDPOINT", "s3-internal.cn-north-1.jdcloud-oss.com");
    c.region = env("VIZ_S3_REGION", "cn-north-1");
    c.accessKey = env("VIZ_S3_ACCESS_KEY", kDefaultAccessKey);
    c.secretKey = env("VIZ_S3_SECRET_KEY", kDefaultSecretKey);
    c.verifySSL = std::string(env("VIZ_S3_VERIFY_SSL", "0")) == "1";
    c.metaApiUrl = env("VIZ_S3_META_API_URL", kDefaultMetaApiUrl);
    return c;
}

// 优先级：环境变量 > JSON 文件 > 内置默认值。文件缺失/解析失败时退回默认。
S3Config configFromJson(const std::string& path) {
    // 1) 内置默认（含默认凭证）。
    S3Config c;
    c.endpoint = "s3-internal.cn-north-1.jdcloud-oss.com";
    c.region ="cn-north-1";
    c.accessKey = kDefaultAccessKey;
    c.secretKey = kDefaultSecretKey;
    c.verifySSL = false;
    c.metaApiUrl = kDefaultMetaApiUrl;

    // 2) JSON 文件覆盖（存在且可解析时）。
    std::ifstream in(path);
    if (in) {
        try {
            nlohmann::json j;
            in >> j;
            c.endpoint = j.value("endpoint", c.endpoint);
            c.region = j.value("region", c.region);
            c.accessKey = j.value("accessKey", c.accessKey);
            c.secretKey = j.value("secretKey", c.secretKey);
            c.verifySSL = j.value("verifySSL", c.verifySSL);
            c.metaApiUrl = j.value("metaApiUrl", c.metaApiUrl);
            c.cacheDir = j.value("cacheDir", c.cacheDir);
        } catch (const std::exception&) {
            // 解析失败：保留默认，交由上层按凭证是否为空判断。
        }
    }

    // 3) 环境变量最高优先级覆盖。
    applyEnvOverride(c);
    return c;
}

// ---- S3Client ------------------------------------------------------------

S3Client::S3Client(S3Config config) : cfg_(std::move(config)) {}

// 执行一次 HTTPS 请求：建立 asio::ssl 连接 -> 构造 SigV4 签名头 -> 发送
// HTTP/1.1 请求 -> 读取并解析响应（status/headers/body）。
S3Client::HttpResponse S3Client::request(const std::string& method,
                                         const std::string& bucket,
                                         const std::string& key,
                                         const std::string& rangeHeader) {
    std::string host, port;
    splitHostPort(cfg_.endpoint, host, port);

    // path-style：/{bucket}/{key}。key 里的 '/' 不编码，其余按 RFC3986。
    std::string canonicalUri = "/" + uriEncode(bucket, false) + "/" +
                               uriEncode(key, false);
    std::string canonicalQuery;  // 只读请求无 query 参数

    AmzTime t = nowAmzTime();
    // 空 body 的 SHA256（GET/HEAD 无请求体）。
    const std::string payloadHash = sha256Hex(std::string());

    // ---- canonical headers（须按 header 名升序，且小写、值去首尾空白）----
    // 参与签名：host, x-amz-content-sha256, x-amz-date。
    std::string canonicalHeaders =
        "host:" + host + "\n" +
        "x-amz-content-sha256:" + payloadHash + "\n" +
        "x-amz-date:" + t.amzDate + "\n";
    const std::string signedHeaders = "host;x-amz-content-sha256;x-amz-date";

    std::string canonicalRequest = method + "\n" + canonicalUri + "\n" +
                                   canonicalQuery + "\n" + canonicalHeaders +
                                   "\n" + signedHeaders + "\n" + payloadHash;

    // ---- string to sign ----
    const std::string service = "s3";
    std::string credentialScope = t.dateStamp + "/" + cfg_.region + "/" +
                                  service + "/aws4_request";
    std::string stringToSign = "AWS4-HMAC-SHA256\n" + t.amzDate + "\n" +
                               credentialScope + "\n" +
                               sha256Hex(canonicalRequest);

    // ---- signing key 派生（HMAC 链）----
    std::string kDate = hmacSha256("AWS4" + cfg_.secretKey, t.dateStamp);
    std::string kRegion = hmacSha256(kDate, cfg_.region);
    std::string kService = hmacSha256(kRegion, service);
    std::string kSigning = hmacSha256(kService, "aws4_request");
    std::string signatureRaw = hmacSha256(kSigning, stringToSign);
    std::string signature = toHex(
        reinterpret_cast<const unsigned char*>(signatureRaw.data()),
        signatureRaw.size());

    std::string authorization =
        "AWS4-HMAC-SHA256 Credential=" + cfg_.accessKey + "/" +
        credentialScope + ", SignedHeaders=" + signedHeaders +
        ", Signature=" + signature;

    // ---- 组装 HTTP 请求 ----
    std::ostringstream req;
    req << method << " " << canonicalUri << " HTTP/1.1\r\n";
    req << "Host: " << host << "\r\n";
    req << "x-amz-date: " << t.amzDate << "\r\n";
    req << "x-amz-content-sha256: " << payloadHash << "\r\n";
    req << "Authorization: " << authorization << "\r\n";
    if (!rangeHeader.empty()) {
        req << "Range: " << rangeHeader << "\r\n";
    }
    req << "Connection: close\r\n";
    req << "\r\n";
    const std::string reqStr = req.str();

    // ---- asio::ssl 连接 + 收发 ----
    asio::io_context io;
    asio::ssl::context ctx(asio::ssl::context::tls_client);
    if (cfg_.verifySSL) {
        ctx.set_default_verify_paths();
        ctx.set_verify_mode(asio::ssl::verify_peer);
    } else {
        ctx.set_verify_mode(asio::ssl::verify_none);
    }

    asio::ssl::stream<asio::ip::tcp::socket> stream(io, ctx);
    // SNI：部分服务端强制要求。
    if (!SSL_set_tlsext_host_name(stream.native_handle(), host.c_str())) {
        throw std::runtime_error("S3: failed to set SNI host");
    }

 asio::ip::tcp::resolver resolver(io);
    auto endpoints = resolver.resolve(host, port);
    asio::connect(stream.next_layer(), endpoints);
    stream.next_layer().set_option(asio::ip::tcp::no_delay(true));
    stream.handshake(asio::ssl::stream_base::client);

    asio::write(stream, asio::buffer(reqStr));

    // ---- 读取全部响应 ----
    std::string raw;
    {
        asio::error_code ec;
        std::array<char, 16384> buf{};
        for (;;) {
            size_t n = stream.read_some(asio::buffer(buf), ec);
            if (n > 0) raw.append(buf.data(), n);
            if (ec == asio::error::eof) break;
            if (ec) {
                // TLS short read（服务端未发 close_notify）视为正常结束。
                if (ec.category() == asio::error::get_ssl_category()) break;
                if (raw.empty())
                    throw std::runtime_error("S3: read error " + ec.message());
                break;
            }
        }
    }
    asio::error_code ignore;
    stream.shutdown(ignore);

    // ---- 解析 status line / headers / body ----
    HttpResponse resp;
    auto headerEnd = raw.find("\r\n\r\n");
    std::string headerBlock =
        headerEnd == std::string::npos ? raw : raw.substr(0, headerEnd);
    std::string bodyStr =
        headerEnd == std::string::npos ? std::string()
                                       : raw.substr(headerEnd + 4);

    // status line: "HTTP/1.1 206 Partial Content"
    {
        auto sp1 = headerBlock.find(' ');
        if (sp1 != std::string::npos) {
            auto sp2 = headerBlock.find(' ', sp1 + 1);
            std::string code = headerBlock.substr(
                sp1 + 1, (sp2 == std::string::npos ? headerBlock.size() : sp2) -
                             sp1 - 1);
            resp.status = std::atoi(code.c_str());
        }
    }
    resp.headers = headerBlock;

    // 若为 chunked 编码则解块；否则原样。此处只读 S3 通常返回 Content-Length，
    // 但 metadata 服务可能 chunked，做一次兜底。
    std::string lowerHdr = headerBlock;
    for (auto& ch : lowerHdr) ch = static_cast<char>(std::tolower(ch));
    if (lowerHdr.find("transfer-encoding: chunked") != std::string::npos) {
        std::string decoded;
        size_t p = 0;
        while (p < bodyStr.size()) {
            auto crlf = bodyStr.find("\r\n", p);
            if (crlf == std::string::npos) break;
            size_t chunkSize =
                std::strtoul(bodyStr.substr(p, crlf - p).c_str(), nullptr, 16);
            p = crlf + 2;
            if (chunkSize == 0) break;
            if (p + chunkSize > bodyStr.size()) chunkSize = bodyStr.size() - p;
            decoded.append(bodyStr, p, chunkSize);
            p += chunkSize + 2;  // 跳过 chunk 尾 CRLF
        }
        bodyStr.swap(decoded);
    }

    resp.body.assign(bodyStr.begin(), bodyStr.end());
    return resp;
}

uint64_t S3Client::headObjectSize(const std::string& bucket,
                                  const std::string& key) {
    HttpResponse resp = request("HEAD", bucket, key, std::string());
    if (resp.status < 200 || resp.status >= 300) {
        throw std::runtime_error("S3 HEAD failed, status=" +
                                 std::to_string(resp.status));
    }
    return parseContentLength(resp.headers);
}

std::vector<uint8_t> S3Client::getRange(const std::string& bucket,
                                        const std::string& key, uint64_t first,
                                        uint64_t last) {
    std::string range =
        "bytes=" + std::to_string(first) + "-" + std::to_string(last);
    HttpResponse resp = request("GET", bucket, key, range);
    // 206 Partial Content 正常；部分服务对整段返回 200。
    if (resp.status != 200 && resp.status != 206) {
        throw std::runtime_error("S3 GET Range failed, status=" +
                                 std::to_string(resp.status));
    }
    return std::move(resp.body);
}

// ---- S3RandomReader ------------------------------------------------------

S3RandomReader::S3RandomReader(std::shared_ptr<S3Client> client,
                               std::string bucket, std::string key)
    : client_(std::move(client)),
      bucket_(std::move(bucket)),
      key_(std::move(key)) {}

uint64_t S3RandomReader::size() const {
    if (!sizeKnown_) {
        size_ = client_->headObjectSize(bucket_, key_);
        sizeKnown_ = true;
    }
    return size_;
}

size_t S3RandomReader::read(uint64_t offset, uint8_t* out, size_t len) {
    if (len == 0) return 0;
    uint64_t total = size();
    if (offset >= total) return 0;
    uint64_t last = offset + len - 1;
    if (last >= total) last = total - 1;
    std::vector<uint8_t> body = client_->getRange(bucket_, key_, offset, last);
    size_t n = body.size();
    if (n > len) n = len;
    std::memcpy(out, body.data(), n);
    return n;
}

}  // namespace viz::s3