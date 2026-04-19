// 冒烟测试:仅用于在测试二进制没有任何真实 TEST 的情况下,保证 gtest 基础
// 设施(链接、入口、ctest 发现)整条链路能跑通。任何时候 ctest 列表里只要
// 看到 Smoke.Passes 一条就说明 gtest 运行时本身是 OK 的。

#include <gtest/gtest.h>

// 场景:最朴素的算术等价,1 + 1 == 2——只为验证 EXPECT_EQ 能跑。
TEST(Smoke, Passes) {
    EXPECT_EQ(1 + 1, 2);
}
