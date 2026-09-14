// 元数据服务客户端：输入 record 名，经明文 HTTP GET 元数据 API 查询该数据包在
// S3 的下载路径（bucket / key）。参考 xmonitor McapAdapter::FetchS3InfoFromMeta。
//
// 元数据 API 约定：
//   GET  <metaBaseUrl><record>
//   Resp {"code":0, "data":"<内嵌 JSON 字符串>"}
//   data 二次解析后取 clipsInfos[0].bucketName / clipsInfos[0].s3Path。
#ifndef VIZ_DATA_META_CLIENT_H
#define VIZ_DATA_META_CLIENT_H

#include <string>

namespace viz::meta {

// 查询 record 对应的 S3 bucket / key。
// metaBaseUrl 形如 "http://host/path?recordName="，会直接拼接 record。
// 成功返回 true 并写入 *bucket / *key；失败（网络错误 / code!=0 / 字段缺失）
// 返回 false，并将错误信息写入 *err（可为 nullptr）。
bool fetchS3InfoFromMeta(const std::string& metaBaseUrl,
                         const std::string& record, std::string* bucket,
                         std::string* key, std::string* err = nullptr);

}  // namespace viz::meta

#endif  // VIZ_DATA_META_CLIENT_H