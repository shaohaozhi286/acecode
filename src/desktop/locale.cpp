#include "locale.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <locale>
#include <nlohmann/json.hpp>

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#endif

namespace acecode::desktop {

namespace {

std::string lowercase_ascii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

bool starts_with(const std::string& value, const std::string& prefix) {
    return value.size() >= prefix.size() &&
           value.compare(0, prefix.size(), prefix) == 0;
}

} // namespace

bool is_supported_locale(const std::string& locale) {
    return locale == kLocaleZhCn || locale == kLocaleEnUs;
}

std::string locale_from_system_tag(const std::string& system_tag) {
    const std::string value = lowercase_ascii(system_tag);
    if (value == "zh" || starts_with(value, "zh-") ||
        starts_with(value, "zh_") || starts_with(value, "chinese")) {
        return kLocaleZhCn;
    }
    return kLocaleEnUs;
}

std::string resolve_ui_locale(const std::string& preference,
                              const std::string& system_tag) {
    if (preference == kLocalePreferenceAuto) {
        return locale_from_system_tag(system_tag);
    }
    return is_supported_locale(preference) ? preference : kLocaleZhCn;
}

std::string detect_system_locale_tag() {
#ifdef _WIN32
    wchar_t value[LOCALE_NAME_MAX_LENGTH] = {};
    const int length = ::GetUserDefaultLocaleName(value, LOCALE_NAME_MAX_LENGTH);
    if (length > 1) {
        std::string out;
        out.reserve(static_cast<std::size_t>(length - 1));
        for (int i = 0; i < length - 1; ++i) {
            const wchar_t ch = value[i];
            out.push_back(ch <= 0x7f ? static_cast<char>(ch) : '?');
        }
        return out;
    }
#else
    for (const char* name : {"LC_ALL", "LC_MESSAGES", "LANG"}) {
        if (const char* value = std::getenv(name); value && *value) {
            return value;
        }
    }
#endif
    try {
        return std::locale("").name();
    } catch (...) {
        return {};
    }
}

std::string locale_bootstrap_script(const std::string& preference,
                                    const std::string& effective_locale) {
    const std::string normalized_preference =
        preference == kLocalePreferenceAuto || is_supported_locale(preference)
            ? preference
            : kLocaleZhCn;
    const std::string normalized_locale = is_supported_locale(effective_locale)
        ? effective_locale
        : resolve_ui_locale(normalized_preference, {});
    return "window.__ACECODE_LOCALE_PREFERENCE__=" +
           nlohmann::json(normalized_preference).dump() + ";\n" +
           "window.__ACECODE_LOCALE__=" + nlohmann::json(normalized_locale).dump() +
           ";\n";
}

} // namespace acecode::desktop
