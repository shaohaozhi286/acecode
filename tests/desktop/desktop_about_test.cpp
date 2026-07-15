#include "desktop/desktop_about.hpp"

#include <gtest/gtest.h>

namespace {

TEST(DesktopAboutTest, FormatsMsvcCompilerVersion) {
    EXPECT_EQ(
        acecode::desktop::format_msvc_compiler_version(1944, 194435213),
        "Microsoft Visual C++ 19.44.35213");
    EXPECT_EQ(
        acecode::desktop::format_msvc_compiler_version(1900, 0),
        "Microsoft Visual C++ 19.00");
}

TEST(DesktopAboutTest, ContentListsAceCodeBrowserAndCompilerVersions) {
    acecode::desktop::DesktopAboutInfo info;
    info.acecode_version = "0.7.1";
    info.browser_name = "WebView2";
    info.browser_version = "138.0.7204.251";
    info.compiler_version = "Microsoft Visual C++ 19.44.35213";

    EXPECT_EQ(
        acecode::desktop::format_desktop_about_content(info),
        "ACECode 版本：v0.7.1\n"
        "浏览器版本：WebView2 138.0.7204.251\n"
        "编译器版本：Microsoft Visual C++ 19.44.35213");
}

TEST(DesktopAboutTest, CurrentCompilerVersionIsAvailable) {
    const std::string version = acecode::desktop::current_compiler_version();
    EXPECT_FALSE(version.empty());
    EXPECT_NE(version, "未知");
}

} // namespace
