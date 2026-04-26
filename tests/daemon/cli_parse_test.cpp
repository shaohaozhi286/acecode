// 覆盖 src/daemon/cli.cpp 的 argv 解析(纯函数,不依赖文件系统/spawn)。
// daemon 子命令是 daemon 模块的"前门",误判会让 start/stop 跑错分支或把
// 用户参数静默吞掉,所以每个分支都要有对应用例兜底。
//
// 端到端的 start→status→stop 走真实进程在 manual QA(已 2026-04-26 验证)。

#include <gtest/gtest.h>

#include "daemon/cli.hpp"

using acecode::daemon::cli::Args;
using acecode::daemon::cli::parse;

// 场景: 仅 "start" → sub=start,无 dangerous/supervised
TEST(DaemonCliParse, BareStart) {
    Args a = parse({"start"});
    EXPECT_EQ(a.sub, "start");
    EXPECT_FALSE(a.dangerous);
    EXPECT_FALSE(a.supervised);
    EXPECT_TRUE(a.error.empty());
}

// 场景: --foreground 单独使用 → sub=foreground(连字符前缀也接受 -foreground)
TEST(DaemonCliParse, ForegroundFlag) {
    EXPECT_EQ(parse({"--foreground"}).sub, "foreground");
    EXPECT_EQ(parse({"-foreground"}).sub, "foreground");
}

// 场景: --foreground 同时给 start → 报错(不允许两个 sub)
TEST(DaemonCliParse, ForegroundConflictsWithStart) {
    Args a = parse({"start", "--foreground"});
    EXPECT_FALSE(a.error.empty()) << "start 和 foreground 不能共存";
}

// 场景: --supervised 必须配 --guid(launcher 派 worker 时必填),否则报错。
// 漏 guid 直接进 worker 会让 GUID 校验失效,这条用例守住启动接口。
TEST(DaemonCliParse, SupervisedRequiresGuid) {
    Args a = parse({"--foreground", "--supervised"});
    EXPECT_FALSE(a.error.empty());
}

// 场景: --supervised + --guid=<G> 正确解析
TEST(DaemonCliParse, SupervisedWithGuid) {
    Args a = parse({"--foreground", "--supervised", "--guid=abc-123"});
    EXPECT_TRUE(a.error.empty());
    EXPECT_TRUE(a.supervised);
    EXPECT_EQ(a.guid, "abc-123");
    EXPECT_EQ(a.sub, "foreground");
}

// 场景: -dangerous 透传给 worker(下游 worker 启动期再做 loopback 互斥校验)
TEST(DaemonCliParse, DangerousFlag) {
    Args a = parse({"start", "-dangerous"});
    EXPECT_TRUE(a.dangerous);
    EXPECT_EQ(a.sub, "start");
}

// 场景: 未知参数必须报错,不能被静默吞掉(否则 typo 会让用户以为命令生效了)
TEST(DaemonCliParse, UnknownArgRejected) {
    Args a = parse({"start", "--turbo"});
    EXPECT_FALSE(a.error.empty());
}

// 场景: 多个子命令冲突 → 报错
TEST(DaemonCliParse, MultipleSubcommandsRejected) {
    Args a = parse({"start", "stop"});
    EXPECT_FALSE(a.error.empty());
}

// 场景: 空 argv → sub 留空(调用方应打印 help 并以非零退出)
TEST(DaemonCliParse, EmptyTokens) {
    Args a = parse({});
    EXPECT_TRUE(a.sub.empty());
    EXPECT_TRUE(a.error.empty());
}
