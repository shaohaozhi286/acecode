#include "terminal_theme_detect.hpp"

#include "logger.hpp"

#include <charconv>
#include <cmath>
#include <cstdlib>
#include <sstream>

namespace acecode {

// ---- OSC 11 响应解析 ----
// 格式: "rgb:RRRR/GGGG/BBBB",每段 1-4 位 hex。
// 归一化到 [0,1] 后按 WCAG 相对亮度公式计算。

std::optional<double> parse_osc11_luminance(const std::string& body) {
    // 跳过可能的 "rgb:" 前缀(有些终端返回带前缀,有些不带)
    std::string_view sv(body);
    if (sv.substr(0, 4) == "rgb:") {
        sv.remove_prefix(4);
    }

    // 按 '/' 分成三段
    auto parse_component = [](std::string_view hex) -> std::optional<double> {
        if (hex.empty() || hex.size() > 4) return std::nullopt;
        unsigned val = 0;
        auto [ptr, ec] = std::from_chars(hex.data(), hex.data() + hex.size(),
                                         val, 16);
        if (ec != std::errc{} || ptr != hex.data() + hex.size()) {
            return std::nullopt;
        }
        // 按位数归一化: 1位→/0xF, 2位→/0xFF, 3位→/0xFFF, 4位→/0xFFFF
        static constexpr unsigned max_by_len[] = {0, 0xF, 0xFF, 0xFFF, 0xFFFF};
        return static_cast<double>(val) / static_cast<double>(max_by_len[hex.size()]);
    };

    auto slash1 = sv.find('/');
    if (slash1 == std::string_view::npos) return std::nullopt;
    auto slash2 = sv.find('/', slash1 + 1);
    if (slash2 == std::string_view::npos) return std::nullopt;

    auto r = parse_component(sv.substr(0, slash1));
    auto g = parse_component(sv.substr(slash1 + 1, slash2 - slash1 - 1));
    auto b = parse_component(sv.substr(slash2 + 1));

    if (!r || !g || !b) return std::nullopt;

    // WCAG 相对亮度
    return 0.2126 * (*r) + 0.7152 * (*g) + 0.0722 * (*b);
}

// ---- COLORFGBG 解析 ----
// 格式: "fg;bg" 或 "fg;extra;bg" — 取最后一段数字。
// ANSI 色号 0-6 视为暗色,7-15 视为亮色。

DetectedTheme theme_from_ansi_background_index(unsigned bg) {
    return bg <= 6 ? DetectedTheme::dark : DetectedTheme::light;
}

std::optional<DetectedTheme> parse_colorfgbg(const std::string& value) {
    if (value.empty()) return std::nullopt;

    // 取最后一个 ';' 之后的部分
    auto last_semi = value.rfind(';');
    std::string_view bg_str;
    if (last_semi != std::string::npos) {
        bg_str = std::string_view(value).substr(last_semi + 1);
    } else {
        return std::nullopt;
    }

    // "default" 视为暗色(常见于某些终端)
    if (bg_str == "default") return DetectedTheme::dark;

    unsigned bg = 0;
    auto [ptr, ec] = std::from_chars(bg_str.data(), bg_str.data() + bg_str.size(),
                                     bg);
    if (ec != std::errc{} || ptr != bg_str.data() + bg_str.size()) {
        return std::nullopt;
    }

    // ANSI 基色 0-6 暗色,7+ 亮色(7=white/silver 视为亮底)
    return theme_from_ansi_background_index(bg);
}

// ---- 探测链(注入式) ----

DetectedTheme detect_terminal_theme_with(
    const std::function<std::optional<std::string>()>& osc11_probe,
    const std::function<std::optional<std::string>(const char*)>& env_lookup,
    const std::function<std::optional<bool>()>& terminal_background_dark) {

    // ① OSC 11
    if (osc11_probe) {
        auto response = osc11_probe();
        if (response) {
            auto lum = parse_osc11_luminance(*response);
            if (lum) {
                auto result = *lum > 0.5 ? DetectedTheme::light : DetectedTheme::dark;
                LOG_INFO("[theme_detect] OSC 11 luminance=" +
                         std::to_string(*lum) + " → " +
                         (result == DetectedTheme::dark ? "dark" : "light"));
                return result;
            }
            LOG_WARN("[theme_detect] OSC 11 response unparseable: " + *response);
        }
    }

    // ② COLORFGBG
    if (env_lookup) {
        auto colorfgbg = env_lookup("COLORFGBG");
        if (colorfgbg) {
            auto result = parse_colorfgbg(*colorfgbg);
            if (result) {
                LOG_INFO("[theme_detect] COLORFGBG='" + *colorfgbg + "' → " +
                         (*result == DetectedTheme::dark ? "dark" : "light"));
                return *result;
            }
        }
    }

    // ③ 平台终端背景属性
    if (terminal_background_dark) {
        auto dark_background = terminal_background_dark();
        if (dark_background) {
            auto result = *dark_background ? DetectedTheme::dark : DetectedTheme::light;
            LOG_INFO("[theme_detect] terminal background dark = " +
                     std::string(*dark_background ? "true" : "false") + " → " +
                     (result == DetectedTheme::dark ? "dark" : "light"));
            return result;
        }
    }

    // ④ 默认
    LOG_INFO("[theme_detect] all probes failed, defaulting to dark");
    return DetectedTheme::dark;
}

// ---- 平台 I/O(在 _win.cpp / _posix.cpp 中提供 probe_osc11 和背景回退) ----

namespace {
std::optional<std::string> default_env_lookup(const char* name) {
    const char* v = std::getenv(name);
    if (!v) return std::nullopt;
    return std::string(v);
}
} // namespace

// 前向声明(平台文件提供)
std::optional<std::string> probe_osc11_platform();
std::optional<bool> probe_terminal_background_dark_mode();

DetectedTheme detect_terminal_theme() {
    return detect_terminal_theme_with(
        probe_osc11_platform,
        default_env_lookup,
        probe_terminal_background_dark_mode);
}

} // namespace acecode
