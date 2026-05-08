// single_instance.{hpp,cpp,_win.cpp,_posix.cpp} 单元测试。
//
// 验收点:
//   - 默认构造的 SingleInstance::acquired() 是 false
//   - 第一次 try_acquire() 返回 true,acquired() 为 true
//   - 同一进程内第二个 SingleInstance::try_acquire() 返回 false(被现有 lock 顶住)
//   - release 后,新建一个 SingleInstance 可以重新获取
//   - destructor 自动 release(下一个 try_acquire 能成功)
//   - focus_existing_instance() 在 POSIX 上 stub 返回 false;Windows 上即使没有
//     已有窗口也会走 BroadcastSystemMessage 路径,返回值不强约束(广播失败为 false,
//     成功为 true) — 我们只断言不崩
//
// 设计:openspec/changes/enhance-desktop-tray-menu(后续追加 single-instance 一节)。

#include <gtest/gtest.h>

#include "desktop/single_instance.hpp"

using namespace acecode::desktop;

TEST(DesktopSingleInstance, DefaultConstructedIsNotAcquired) {
    SingleInstance s;
    EXPECT_FALSE(s.acquired());
}

TEST(DesktopSingleInstance, FirstAcquireSucceeds) {
    SingleInstance s;
    EXPECT_TRUE(s.try_acquire());
    EXPECT_TRUE(s.acquired());
}

TEST(DesktopSingleInstance, SecondInstanceCannotAcquireWhileFirstHolds) {
    SingleInstance first;
    ASSERT_TRUE(first.try_acquire());

    SingleInstance second;
    EXPECT_FALSE(second.try_acquire());
    EXPECT_FALSE(second.acquired());
}

TEST(DesktopSingleInstance, ExplicitReleaseAllowsNewAcquire) {
    SingleInstance first;
    ASSERT_TRUE(first.try_acquire());
    first.release();
    EXPECT_FALSE(first.acquired());

    SingleInstance second;
    EXPECT_TRUE(second.try_acquire());
    EXPECT_TRUE(second.acquired());
}

TEST(DesktopSingleInstance, DestructorReleasesAllowingReacquire) {
    {
        SingleInstance scoped;
        ASSERT_TRUE(scoped.try_acquire());
    }
    SingleInstance fresh;
    EXPECT_TRUE(fresh.try_acquire());
}

TEST(DesktopSingleInstance, RepeatedAcquireOnSameInstanceIsIdempotent) {
    SingleInstance s;
    EXPECT_TRUE(s.try_acquire());
    EXPECT_TRUE(s.try_acquire());  // 第二次是 no-op,不重复 acquire
    EXPECT_TRUE(s.acquired());
}

TEST(DesktopSingleInstance, FocusExistingInstanceDoesNotCrash) {
    // 当前测试进程根本没创建 host window,所以 Windows 端可能 broadcast 失败、
    // POSIX 端 stub 直接 false。两端都不应崩。
    bool ok = focus_existing_instance();
    (void)ok; // 不强约束返回值
    SUCCEED();
}
