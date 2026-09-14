// bigdata_ring_test: BigDataRing 字节预算 LRU + 逐出最旧 单测。
// 仅依赖纯 header session/bigdata_ring.h，不牵扯 viz-core 重依赖。
#include "session/bigdata_ring.h"

#include <string>

#include <gtest/gtest.h>

// 超字节预算时逐出最久未用（back）项。
TEST(BigDataRing, EvictsOldestWhenOverBudget) {
  viz::BigDataRing ring(/*budgetBytes=*/100);
  ring.Put("a", std::string(60, 'x'));
  ring.Put("b", std::string(60, 'y'));  // 60+60=120 > 100，逐出最旧 a
  EXPECT_FALSE(ring.Has("a"));
  EXPECT_TRUE(ring.Has("b"));
}

// Get 命中把项提到最近（front），后续逐出的是次旧项。
TEST(BigDataRing, TouchOnGetKeepsRecent) {
  viz::BigDataRing ring(120);
  ring.Put("a", std::string(50, 'x'));
  ring.Put("b", std::string(50, 'y'));
  ring.Get("a");                        // a 变最近，b 成最旧
ring.Put("c", std::string(50, 'z'));  // 150 > 120，逐出最旧 b
  EXPECT_TRUE(ring.Has("a"));
  EXPECT_FALSE(ring.Has("b"));
  EXPECT_TRUE(ring.Has("c"));
}

// 同 key 重复 Put 覆盖旧值、更新字节数、不重复计入。
TEST(BigDataRing, OverwriteSameKeyUpdatesBytes) {
  viz::BigDataRing ring(100);
  ring.Put("a", std::string(30, 'x'));
  ring.Put("a", std::string(90, 'y'));  // 覆盖，字节数应为 90 而非 120
  EXPECT_TRUE(ring.Has("a"));
  const std::string* v = ring.Get("a");
  ASSERT_NE(v, nullptr);
  EXPECT_EQ(v->size(), 90u);
}