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
    snap.ca_bundle = "";
    snap.insecure = false;

    std::string out = format_proxy_display(snap);
    EXPECT_NE(out.find("Effective proxy : direct"), std::string::npos);
    EXPECT_NE(out.find("Source          : mode=off"), std::string::npos);
    EXPECT_NE(out.find("Mode (config)   : off"), std::string::npos);
    EXPECT_NE(out.find("CA bundle       : (none)"), std::string::npos);
    EXPECT_NE(out.find("Skip TLS verify : no"), std::string::npos);
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

// 场景:insecure_skip_verify=true 在 display 文案里要显示 "yes",方便用户
// 立刻看出"哎我开了 TLS 跳过校验"。
TEST(ProxyDisplay, InsecureFlagShowsYes) {
    ProxyDisplaySnapshot snap;
    snap.effective_url = "http://127.0.0.1:8888";
    snap.source = "windows-system";
    snap.mode = "auto";
    snap.insecure = true;

    std::string out = format_proxy_display(snap);
    EXPECT_NE(out.find("Skip TLS verify : yes"), std::string::npos);
}

// 场景:CA bundle 路径。非空时直接展示路径(让用户检查路径是否正确)。
TEST(ProxyDisplay, ShowsCaBundlePathWhenSet) {
    ProxyDisplaySnapshot snap;
    snap.effective_url = "http://127.0.0.1:8888";
    snap.source = "manual";
    snap.mode = "manual";
    snap.ca_bundle = "C:\\Users\\me\\FiddlerRoot.pem";

    std::string out = format_proxy_display(snap);
    EXPECT_NE(out.find("CA bundle       : C:\\Users\\me\\FiddlerRoot.pem"),
              std::string::npos);
}
