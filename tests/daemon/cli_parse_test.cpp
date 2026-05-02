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

// 场景: --port=N 正常解析,与 --foreground 共用(desktop 父进程预选空闲端口注入子进程)
TEST(DaemonCliParse, PortOverride) {
    Args a = parse({"--foreground", "--port=49321"});
    EXPECT_TRUE(a.error.empty());
    EXPECT_EQ(a.port_override, 49321);
    EXPECT_EQ(a.sub, "foreground");
}

// 场景: --port=0 越界,必须报错(0 不是合法绑定端口,且会与"未指定"语义混淆)
TEST(DaemonCliParse, PortZeroRejected) {
    Args a = parse({"--foreground", "--port=0"});
    EXPECT_FALSE(a.error.empty());
}

// 场景: --port=65536 越界,必须报错(uint16 上限 65535)
TEST(DaemonCliParse, PortOutOfRange) {
    Args a = parse({"--foreground", "--port=65536"});
    EXPECT_FALSE(a.error.empty());
}

// 场景: --port=foo 非整数,必须报错(否则 stoi 抛异常会让 parse 崩,这里走兜底)
TEST(DaemonCliParse, PortNotInteger) {
    Args a = parse({"--foreground", "--port=abc"});
    EXPECT_FALSE(a.error.empty());
}

// 场景: --token=<T> 正常解析(desktop 父进程预生成 token,子进程不再走 generate_auth_token)
TEST(DaemonCliParse, TokenOverride) {
    Args a = parse({"--foreground", "--token=secret-xyz-123"});
    EXPECT_TRUE(a.error.empty());
    EXPECT_EQ(a.token_override, "secret-xyz-123");
}

// 场景: --token= 空字符串必须报错(若静默接受会让 worker.cpp 写出空 token 文件)
TEST(DaemonCliParse, TokenEmptyRejected) {
    Args a = parse({"--foreground", "--token="});
    EXPECT_FALSE(a.error.empty());
}

// 场景: --port + --token 同时存在,模拟 desktop 完整调用形式
TEST(DaemonCliParse, PortAndTokenCombined) {
    Args a = parse({"--foreground", "--port=12345", "--token=abc"});
    EXPECT_TRUE(a.error.empty());
    EXPECT_EQ(a.port_override, 12345);
    EXPECT_EQ(a.token_override, "abc");
    EXPECT_EQ(a.sub, "foreground");
}

// 场景: --cwd=<path> 正常解析,让 daemon 从任意启动目录服务指定 workspace。
TEST(DaemonCliParse, CwdOverrideEqualsForm) {
    Args a = parse({"--foreground", "--cwd=C:\\Users\\shao\\shzdebug"});
    EXPECT_TRUE(a.error.empty());
    EXPECT_EQ(a.cwd_override, "C:\\Users\\shao\\shzdebug");
    EXPECT_EQ(a.sub, "foreground");
}

// 场景: --cwd <path> 空格形式也支持,方便路径由脚本变量传入。
TEST(DaemonCliParse, CwdOverrideSplitForm) {
    Args a = parse({"--foreground", "--cwd", "C:\\Users\\shao\\shzdebug"});
    EXPECT_TRUE(a.error.empty());
    EXPECT_EQ(a.cwd_override, "C:\\Users\\shao\\shzdebug");
}

// 场景: --cwd 缺 path 必须报错,不能让 worker 悄悄用 process cwd。
TEST(DaemonCliParse, CwdOverrideMissingPathRejected) {
    Args a = parse({"--foreground", "--cwd"});
    EXPECT_FALSE(a.error.empty());
}

// 场景: --cwd= 空字符串必须报错。
TEST(DaemonCliParse, CwdOverrideEmptyRejected) {
    Args a = parse({"--foreground", "--cwd="});
    EXPECT_FALSE(a.error.empty());
}
