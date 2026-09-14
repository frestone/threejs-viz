#pragma once
// 大数据(图像/RawData)预解码结果的后端有界环形缓冲：字节预算 LRU。
// 围绕 playhead 前瞻预解码的产物暂存于此，超预算逐出最久未用项(back)。
// 非线程安全，调用方(BigDataRun)在持锁下使用。
#include <cstddef>
#include <list>
#include <string>
#include <unordered_map>
#include <utility>

namespace viz {

class BigDataRing {
 public:
  explicit BigDataRing(size_t budgetBytes) : budget_(budgetBytes) {}

  // 写入/覆盖 key。已存在则先扣旧字节并从顺序链移除，再作为最近项插入。
  void Put(const std::string& key, std::string value) {
    auto it = map_.find(key);
    if (it != map_.end()) {
      bytes_ -= it->second.value.size();
      order_.erase(it->second.pos);
      map_.erase(it);
    }
    order_.push_front(key);
    bytes_ += value.size();
    map_.emplace(key, Entry{std::move(value), order_.begin()});
    EvictToBudget();
  }

  // 命中则把该项提到最近(front)并返回值指针；未命中返回 nullptr。
  const std::string* Get(const std::string& key) {
    auto it = map_.find(key);
    if (it == map_.end()) return nullptr;
    order_.erase(it->second.pos);
    order_.push_front(key);
    it->second.pos = order_.begin();
    return &it->second.value;
  }

  bool Has(const std::string& key) const { return map_.count(key) > 0; }

  size_t Bytes() const { return bytes_; }

 private:
  struct Entry {
    std::string value;
    std::list<std::string>::iterator pos;
  };

  // 逐出最久未用项(order_ back)直到不超预算。
  void EvictToBudget() {
    while (bytes_ > budget_ && !order_.empty()) {
      auto it = map_.find(order_.back());
      if (it != map_.end()) {
        bytes_ -= it->second.value.size();
        map_.erase(it);
      }
      order_.pop_back();
    }
  }

  size_t budget_;
  size_t bytes_ = 0;
  std::list<std::string> order_;  // front=最近, back=最旧
  std::unordered_map<std::string, Entry> map_;
};

}  // namespace viz