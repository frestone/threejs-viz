// Frame 数据模型 + proto3 序列化,严格对应 public/frame.proto (package viz)。
// map<string,V> 按 proto3 约定序列化为 repeated MapEntry{key=1, value=2}。
#pragma once

#include <string>
#include <vector>

#include "proto_writer.h"

namespace model {

// frame.proto GeometryKind
enum GeometryKind { POINT = 0, LINESTRIP = 1, BOX = 2, POLYGON = 3, TEXT = 4, ARROW = 5, PLANNING_TRAJECTORY = 6 };
// frame.proto ChartSeriesKind
enum ChartSeriesKind { CS_LINE = 0, CS_SCATTER = 1, CS_BAND_UPPER = 2, CS_BAND_LOWER = 3 };

struct Vec3 {
  float x = 0, y = 0, z = 0;
};

struct GeometryItem {
  Vec3 position;
  bool hasPosition = false;
  std::vector<Vec3> points;   // LINESTRIP / POLYGON
  Vec3 size;
  bool hasSize = false;       // BOX
  float heading = 0;
  std::string text;           // TEXT
  int id = 0;
  int type = 0;
  float score = 0;
};

struct LayerData {
  GeometryKind kind = POINT;
  std::vector<GeometryItem> items;
};

// frame.proto Image：相机/传感器画面。
struct Image {
  std::string format;       // "jpeg"/"png"/"rgb8"
  unsigned width = 0;
  unsigned height = 0;
  std::string data;         // 编码字节（bytes）
  double t = 0;
};

struct ChartSeries {
  std::string name;
  std::vector<float> x;
  std::vector<float> y;
  ChartSeriesKind kind = CS_LINE;
  std::string color;
};

struct ChartData {
  std::string title;
  std::string xLabel;
  std::string yLabel;
  std::vector<ChartSeries> series;
};

struct Frame {
  double t = 0;
  std::vector<std::pair<std::string, LayerData>> layers;
  Vec3 egoAnchor;
  float egoYaw = 0;
  bool egoValid = false;
  std::vector<std::pair<std::string, ChartData>> charts;
  std::vector<std::pair<std::string, Image>> images;
};

// ---- 序列化 ----

inline std::string serializeVec3(const Vec3& v) {
  std::string out;
  if (v.x != 0) pw::putFloat(out, 1, v.x);
  if (v.y != 0) pw::putFloat(out, 2, v.y);
  if (v.z != 0) pw::putFloat(out, 3, v.z);
  return out;
}

inline std::string serializeGeometryItem(const GeometryItem& g) {
  std::string out;
  if (g.hasPosition) pw::putBytes(out, 1, serializeVec3(g.position));
  for (const auto& p : g.points) pw::putBytes(out, 2, serializeVec3(p));
  if (g.hasSize) pw::putBytes(out, 3, serializeVec3(g.size));
  if (g.heading != 0) pw::putFloat(out, 4, g.heading);
  if (!g.text.empty()) pw::putString(out, 5, g.text);
  if (g.id != 0) pw::putInt32(out, 6, g.id);
  if (g.type != 0) pw::putInt32(out, 7, g.type);
  if (g.score != 0) pw::putFloat(out, 8, g.score);
  return out;
}

inline std::string serializeLayerData(const LayerData& l) {
  std::string out;
  if (l.kind != POINT) pw::putEnum(out, 1, l.kind);
  for (const auto& it : l.items) pw::putBytes(out, 2, serializeGeometryItem(it));
  return out;
}

inline std::string serializeChartSeries(const ChartSeries& s) {
  std::string out;
  if (!s.name.empty()) pw::putString(out, 1, s.name);
  pw::putPackedFloat(out, 2, s.x.data(), s.x.size());
  pw::putPackedFloat(out, 3, s.y.data(), s.y.size());
  if (s.kind != CS_LINE) pw::putEnum(out, 4, s.kind);
  if (!s.color.empty()) pw::putString(out, 5, s.color);
  return out;
}

inline std::string serializeChartData(const ChartData& c) {
  std::string out;
  if (!c.title.empty()) pw::putString(out, 1, c.title);
  if (!c.xLabel.empty()) pw::putString(out, 2, c.xLabel);
  if (!c.yLabel.empty()) pw::putString(out, 3, c.yLabel);
  for (const auto& s : c.series) pw::putBytes(out, 4, serializeChartSeries(s));
  return out;
}

// map<string, LayerData> entry: key=1(string), value=2(message)
inline std::string serializeLayerEntry(const std::string& key, const LayerData& v) {
  std::string out;
  pw::putString(out, 1, key);
  pw::putBytes(out, 2, serializeLayerData(v));
  return out;
}

inline std::string serializeChartEntry(const std::string& key, const ChartData& v) {
  std::string out;
  pw::putString(out, 1, key);
  pw::putBytes(out, 2, serializeChartData(v));
  return out;
}

// map<string, Image> entry: key=1(string), value=2(message)
inline std::string serializeImage(const Image& im) {
  std::string out;
  if (!im.format.empty()) pw::putString(out, 1, im.format);
  if (im.width != 0) pw::putInt32(out, 2, static_cast<int32_t>(im.width));
  if (im.height != 0) pw::putInt32(out, 3, static_cast<int32_t>(im.height));
  if (!im.data.empty()) pw::putBytes(out, 4, im.data);
  if (im.t != 0) pw::putDouble(out, 5, im.t);
  return out;
}

inline std::string serializeImageEntry(const std::string& key, const Image& v) {
  std::string out;
  pw::putString(out, 1, key);
  pw::putBytes(out, 2, serializeImage(v));
  return out;
}

inline std::string serializeFrame(const Frame& f) {
  std::string out;
  if (f.t != 0) pw::putDouble(out, 1, f.t);
  for (const auto& kv : f.layers) pw::putBytes(out, 2, serializeLayerEntry(kv.first, kv.second));
  if (f.egoValid || f.egoAnchor.x != 0 || f.egoAnchor.y != 0 || f.egoAnchor.z != 0)
    pw::putBytes(out, 3, serializeVec3(f.egoAnchor));
  if (f.egoYaw != 0) pw::putFloat(out, 4, f.egoYaw);
  if (f.egoValid) pw::putBool(out, 5, true);
  for (const auto& kv : f.charts) pw::putBytes(out, 6, serializeChartEntry(kv.first, kv.second));
  for (const auto& kv : f.images) pw::putBytes(out, 7, serializeImageEntry(kv.first, kv.second));
  return out;
}

}  // namespace model