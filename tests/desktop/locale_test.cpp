#include "desktop/locale.hpp"

#include <gtest/gtest.h>

namespace acecode::desktop {

TEST(DesktopLocale, ResolvesChineseSystemTags) {
    EXPECT_EQ(locale_from_system_tag("zh-CN"), "zh-CN");
    EXPECT_EQ(locale_from_system_tag("zh_Hant_TW.UTF-8"), "zh-CN");
    EXPECT_EQ(locale_from_system_tag("Chinese (Simplified)_China.936"), "zh-CN");
}

TEST(DesktopLocale, ResolvesNonChineseAndEmptySystemTagsToEnglish) {
    EXPECT_EQ(locale_from_system_tag("en-US"), "en-US");
    EXPECT_EQ(locale_from_system_tag("de_DE.UTF-8"), "en-US");
    EXPECT_EQ(locale_from_system_tag(""), "en-US");
}

TEST(DesktopLocale, ExplicitPreferenceWinsAndInvalidPreservesLegacyChinese) {
    EXPECT_EQ(resolve_ui_locale("en-US", "zh-CN"), "en-US");
    EXPECT_EQ(resolve_ui_locale("zh-CN", "en-US"), "zh-CN");
    EXPECT_EQ(resolve_ui_locale("auto", "en-GB"), "en-US");
    EXPECT_EQ(resolve_ui_locale("fr-FR", "en-US"), "zh-CN");
}

TEST(DesktopLocale, BootstrapScriptPublishesNormalizedValuesBeforeMount) {
    EXPECT_EQ(locale_bootstrap_script("auto", "en-US"),
              "window.__ACECODE_LOCALE_PREFERENCE__=\"auto\";\n"
              "window.__ACECODE_LOCALE__=\"en-US\";\n");
    EXPECT_EQ(locale_bootstrap_script("invalid", "invalid"),
              "window.__ACECODE_LOCALE_PREFERENCE__=\"zh-CN\";\n"
              "window.__ACECODE_LOCALE__=\"zh-CN\";\n");
}

} // namespace acecode::desktop
