// 覆盖 src/commands/proxy_command.cpp 的纯文案构造函数 format_proxy_display。
// /proxy 命令本体改 ctx.state 状态,需要 mock 一整个 CommandContext —— 不在
// 单测层做。这个测试聚焦"用户视角看到的字符串结构是否稳定",能在新人改文案
// 时立刻发现意外破坏(比如把 effective url 换成 redact 调用前的版本)。

#include <gtest/gtest.h>

#include "commands/proxy_command.hpp"

using acecode::ProxyDisplaySnapshot;
using acecode::format_proxy_display;

// 场景:直连。effective_url 为空时必须显示 "direct" 而不是空字符串。
TEST(ProxyDisplay, DirectConnectionShowsDirectKeyword) {
    ProxyDisplaySnapshot snap;
    snap.effective_url = "";
    snap.source = "mode=off";
    snap.mode = "off";

    std::string out = format_proxy_display(snap);
    EXPECT_NE(out.find("Effective proxy : direct"), std::string::npos);
    EXPECT_NE(out.find("Source          : mode=off"), std::string::npos);
    EXPECT_NE(out.find("Mode (config)   : off"), std::string::npos);
    EXPECT_EQ(out.find("CA bundle"), std::string::npos);
    EXPECT_EQ(out.find("Skip TLS verify"), std::string::npos);
}

// 场景:有代理 + 凭据。密码必须被 *** 替换,user 名保留。
TEST(ProxyDisplay, RedactsPasswordsInEffectiveUrl) {
    ProxyDisplaySnapshot snap;
    snap.effective_url = "http://alice:secret@proxy.corp:8080";
    snap.source = "manual";
    snap.mode = "manual";

    std::string out = format_proxy_display(snap);
    EXPECT_NE(out.find("alice:***@proxy.corp:8080"), std::string::npos);
    EXPECT_EQ(out.find("secret"), std::string::npos);
}

// 场景(proxy-fallback-on-unreachable):reachable=true 时显示 "Reachable : yes"
// 不带 Original proxy 行,保留旧 display 信息密度。
TEST(ProxyDisplay, ReachableYesShowsSimpleLine) {
    ProxyDisplaySnapshot snap;
    snap.effective_url = "http://127.0.0.1:8888";
    snap.source = "manual";
    snap.mode = "manual";
    snap.reachable = true;

    std::string out = format_proxy_display(snap);
    EXPECT_NE(out.find("Reachable       : yes"), std::string::npos);
    EXPECT_EQ(out.find("Original proxy"), std::string::npos);
}

// 场景:fallback 激活时 "Reachable : no (Refused)" + "Original proxy : ..." 出现
TEST(ProxyDisplay, ReachableNoShowsReasonAndOriginalProxy) {
    ProxyDisplaySnapshot snap;
    snap.effective_url = ""; // 已经走直连了
    snap.source = "auto-fallback";
    snap.mode = "manual";
    snap.reachable = false;
    snap.reachable_reason = "Refused";
    snap.original_proxy_url = "http://127.0.0.1:8888";
    snap.original_proxy_source = "manual";

    std::string out = format_proxy_display(snap);
    EXPECT_NE(out.find("Reachable       : no (Refused)"), std::string::npos);
    EXPECT_NE(out.find("Original proxy  : http://127.0.0.1:8888 (manual)"),
              std::string::npos);
    EXPECT_NE(out.find("Effective proxy : direct"), std::string::npos);
    EXPECT_NE(out.find("Source          : auto-fallback"), std::string::npos);
}

// 场景:fallback 时凭据脱敏后才进入 Original proxy 行(snapshot 已脱敏)
TEST(ProxyDisplay, OriginalProxyUrlAlreadyRedacted) {
    ProxyDisplaySnapshot snap;
    snap.source = "auto-fallback";
    snap.mode = "manual";
    snap.reachable = false;
    snap.reachable_reason = "Timeout";
    // 调用方(snapshot_for_display)负责脱敏;display 函数不再二次处理,直接展示。
    snap.original_proxy_url = "http://alice:***@proxy.corp:8080";
    snap.original_proxy_source = "windows-system";

    std::string out = format_proxy_display(snap);
    EXPECT_NE(out.find("alice:***@proxy.corp:8080"), std::string::npos);
    EXPECT_EQ(out.find(":secret"), std::string::npos);
}
