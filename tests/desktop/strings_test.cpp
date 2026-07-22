#include "desktop/strings.hpp"

#include <gtest/gtest.h>

#include <string>

namespace acecode::desktop {

TEST(DesktopStrings, BothCatalogsCoverEveryTypedEntry) {
    for (std::size_t index = 0;
         index < static_cast<std::size_t>(DesktopStringId::Count);
         ++index) {
        const auto id = static_cast<DesktopStringId>(index);
        EXPECT_FALSE(desktop_string(id, "zh-CN").empty()) << index;
        EXPECT_FALSE(desktop_string(id, "en-US").empty()) << index;
    }
    EXPECT_EQ(desktop_string(DesktopStringId::TrayQuit, "invalid"), "退出");
}

TEST(DesktopStrings, RuntimeLocaleStateNormalizesAndSwitches) {
    set_native_locale("en-US");
    EXPECT_EQ(native_locale(), "en-US");
    EXPECT_EQ(native_string(DesktopStringId::FolderPickerTitle),
              "Select project folder");
    set_native_locale("bad");
    EXPECT_EQ(native_locale(), "zh-CN");
    set_native_locale("zh-CN");
}

TEST(DesktopStrings, StartupMessagesLocalizeShellAndPreserveDiagnostics) {
    const std::string english = format_browser_fallback_open_failed_message(
        "WebView unavailable", "用户浏览器错误", "0x80070002", "en-US");
    EXPECT_NE(english.find("browser fallback also failed"), std::string::npos);
    EXPECT_NE(english.find("用户浏览器错误"), std::string::npos);
    EXPECT_NE(english.find("0x80070002"), std::string::npos);

    const std::string startup = format_startup_exception_message(
        "磁盘错误", "en-US");
    EXPECT_NE(startup.find("unexpected startup error"), std::string::npos);
    EXPECT_NE(startup.find("磁盘错误"), std::string::npos);
}

TEST(DesktopStrings, FolderPickerAndDaemonFailuresUseSelectedLocale) {
    EXPECT_EQ(desktop_string(DesktopStringId::FolderPickerPrompt, "zh-CN"),
              "选择");
    EXPECT_EQ(desktop_string(DesktopStringId::FolderPickerPrompt, "en-US"),
              "Select");
    EXPECT_NE(format_daemon_workspace_failed_message(
                  u8"用户项目", "exit 1", "en-US").find(u8"用户项目"),
              std::string::npos);
}

} // namespace acecode::desktop
