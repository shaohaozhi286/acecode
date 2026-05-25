#include <gtest/gtest.h>

#include "desktop/external_url.hpp"

#include <string>

using acecode::desktop::is_safe_external_url;
using acecode::desktop::open_external_url;

TEST(DesktopExternalUrl, AllowsOnlyHttpAndHttpsUrls) {
    EXPECT_TRUE(is_safe_external_url("https://github.com/login/device"));
    EXPECT_TRUE(is_safe_external_url("http://127.0.0.1:28080/"));
    EXPECT_TRUE(is_safe_external_url("Https://github.com/login/device"));

    EXPECT_FALSE(is_safe_external_url(""));
    EXPECT_FALSE(is_safe_external_url("github.com/login/device"));
    EXPECT_FALSE(is_safe_external_url("file:///C:/Windows/System32/calc.exe"));
    EXPECT_FALSE(is_safe_external_url("javascript:alert(1)"));
}

TEST(DesktopExternalUrl, UsesInjectedLauncherAfterValidation) {
    std::string launched;
    auto result = open_external_url(
        "https://github.com/login/device",
        [&](const std::string& url, std::string&) {
            launched = url;
            return true;
        });

    EXPECT_TRUE(result.ok) << result.error;
    EXPECT_EQ(launched, "https://github.com/login/device");
}

TEST(DesktopExternalUrl, RejectsUnsafeUrlBeforeLauncher) {
    bool called = false;
    auto result = open_external_url(
        "file:///tmp/secret",
        [&](const std::string&, std::string&) {
            called = true;
            return true;
        });

    EXPECT_FALSE(result.ok);
    EXPECT_FALSE(called);
}

TEST(DesktopExternalUrl, PropagatesLauncherFailure) {
    auto result = open_external_url(
        "https://github.com/login/device",
        [](const std::string&, std::string& error) {
            error = "mock failure";
            return false;
        });

    EXPECT_FALSE(result.ok);
    EXPECT_EQ(result.error, "mock failure");
}
