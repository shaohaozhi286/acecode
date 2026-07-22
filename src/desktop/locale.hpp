#pragma once

#include <string>

namespace acecode::desktop {

inline constexpr const char* kLocalePreferenceAuto = "auto";
inline constexpr const char* kLocaleZhCn = "zh-CN";
inline constexpr const char* kLocaleEnUs = "en-US";

bool is_supported_locale(const std::string& locale);
std::string locale_from_system_tag(const std::string& system_tag);
std::string resolve_ui_locale(const std::string& preference,
                              const std::string& system_tag);

// Best-effort OS locale tag. The resolver deliberately needs only a Chinese
// vs non-Chinese distinction, so unusual/empty results safely become en-US.
std::string detect_system_locale_tag();

// Script fragment executed before any WebUI module. JSON string encoding is
// used so platform locale values cannot escape into JavaScript source.
std::string locale_bootstrap_script(const std::string& preference,
                                    const std::string& effective_locale);

} // namespace acecode::desktop
