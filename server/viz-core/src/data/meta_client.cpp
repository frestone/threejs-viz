// 元数据服务客户端实现：明文 HTTP/1.1 GET + 两层 JSON 解析。见 meta_client.h。
#include "data/meta_client.h"

#include <cctype>
#include <cstring>
#include <sstream>
#include <stdexcept>

#include <nlohmann/json.hpp>

// standalone Asio（非 Boost）。ASIO_STANDALONE 由 BUILD defines 传入。
#include <asio.hpp>

namespace viz::meta {
namespace {

// 解析形如 http://host[:port]/path 的 URL。仅支持明文 http。
struct ParsedUrl {
    std::string host;
    std::string port;  // 默认 "80"
    std::string path;  // 含前导 '/'，可含 query
};

bool parseUrl(const std::string& url, ParsedUrl* out, std::string* err) {
    const std::string kPrefix = "http://";
    if (url.compare(0, kPrefix.size(), kPrefix) != 0) {
        if (err) *err = "meta url 必须以 http:// 开头: " + url;
        return false;
    }
    std::string rest = url.substr(kPrefix.size());
    auto slash = rest.find('/');
    std::string authority =
        slash == std::string::npos ? rest : rest.substr(0, slash);
    out->path = slash == std::string::npos ? "/" : rest.substr(slash);
    auto colon = authority.find(':');
    if (colon == std::string::npos) {
        out->host = authority;
        out->port = "80";
    } else {
        out->host = authority.substr(0, colon);
        out->port = authority.substr(colon + 1);
    }
    if (out->host.empty()) {
        if (err) *err = "meta url 缺少主机名: " + url;
        return false;
    }
    return true;
}

// 明文 HTTP GET，返回响应体（已剥离头与 chunked 编码）。失败抛异常。
std::string httpGet(const std::string& url) {
    ParsedUrl u;
    std::string perr;
    if (!parseUrl(url, &u, &perr)) throw std::runtime_error(perr);

    asio::io_context io;
    asio::ip::tcp::resolver resolver(io);
    auto endpoints = resolver.resolve(u.host, u.port);
    asio::ip::tcp::socket socket(io);
    asio::connect(socket, endpoints);

    std::ostringstream req;
    req << "GET " << u.path << " HTTP/1.1\r\n"
        << "Host: " << u.host << "\r\n"
        << "User-Agent: viz-core/1.0\r\n"
        << "Accept: application/json\r\n"
        << "Connection: close\r\n\r\n";
    const std::string reqStr = req.str();
    asio::write(socket, asio::buffer(reqStr));

    std::string raw;
  std::array<char, 4096> buf{};
    asio::error_code ec;
    for (;;) {
        std::size_t n = socket.read_some(asio::buffer(buf), ec);
        if (n > 0) raw.append(buf.data(), n);
        if (ec == asio::error::eof) break;
        if (ec) throw std::runtime_error("meta http 读取失败: " + ec.message());
    }

    // 分割 header / body。
    auto sep = raw.find("\r\n\r\n");
    if (sep == std::string::npos)
        throw std::runtime_error("meta http 响应无有效头部");
    std::string headers = raw.substr(0, sep);
    std::string body = raw.substr(sep + 4);

    // 校验状态行。
    auto sp = headers.find(' ');
    if (sp != std::string::npos) {
        int status = std::atoi(headers.c_str() + sp + 1);
        if (status < 200 || status >= 300)
            throw std::runtime_error("meta http 状态码非 2xx: " +
                                     std::to_string(status));
    }

    // chunked 解码（大小写不敏感检测）。
    std::string lower(headers.size(), '\0');
    for (size_t i = 0; i < headers.size(); ++i)
        lower[i] = static_cast<char>(std::tolower(headers[i]));
    if (lower.find("transfer-encoding: chunked") != std::string::npos) {
        std::string decoded;
        size_t pos = 0;
        while (pos < body.size()) {
            auto crlf = body.find("\r\n", pos);
            if (crlf == std::string::npos) break;
            size_t chunkLen =
                std::strtoul(body.substr(pos, crlf - pos).c_str(), nullptr, 16);
            if (chunkLen == 0) break;
            pos = crlf + 2;
            if (pos + chunkLen > body.size()) break;
            decoded.append(body, pos, chunkLen);
            pos += chunkLen + 2;  // 跳过块尾 CRLF
        }
        return decoded;
    }
    return body;
}

}  // namespace

bool fetchS3InfoFromMeta(const std::string& metaBaseUrl,
                         const std::string& record, std::string* bucket,
                         std::string* key, std::string* err) {
    if (record.empty()) {
        if (err) *err = "record 名为空";
        return false;
    }
    try {
        const std::string body = httpGet(metaBaseUrl + record);
        nlohmann::json root = nlohmann::json::parse(body);
        if (root.value("code", -1) != 0) {
            if (err)
                *err = "meta 返回 code!=0: " + root.value("msg", std::string());
            return false;
        }
        // data 为内嵌 JSON 字符串，需二次解析。
        const std::string dataStr = root.value("data", std::string());
        if (dataStr.empty()) {
            if (err) *err = "meta 返回 data 为空";
            return false;
        }
        nlohmann::json data = nlohmann::json::parse(dataStr);
        const auto& clips = data["clipsInfos"];
        if (!clips.is_array() || clips.empty()) {
            if (err) *err = "meta 返回 clipsInfos 为空";
            return false;
        }
        const auto& c0 = clips[0];
        *bucket = c0.value("bucketName", std::string());
        *key = c0.value("s3Path", std::string());
        if (bucket->empty() || key->empty()) {
            if (err) *err = "meta 返回 bucketName/s3Path 缺失";
            return false;
        }
        return true;
    } catch (const std::exception& e) {
        if (err) *err = std::string("meta 查询异常: ") + e.what();
        return false;
    }
}

}  // namespace viz::meta