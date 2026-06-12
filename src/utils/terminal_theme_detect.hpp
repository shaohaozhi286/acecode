#pragma once

// 终端背景色自动检测 — 判断当前终端是暗色还是亮色背景。
//
// 探测链(按优先级):
//   ① OSC 11 查询(200ms 超时) — 大部分现代终端支持
//   ② COLORFGBG 环境变量 — rxvt/tmux 等设置
//   ③ Windows 系统暗色模式(注册表 AppsUseLightTheme)
//   ④ 默认 dark
//
// 探测必须在 FTXUI 初始化之前完成(FTXUI 接管 stdin 后 OSC 读不到)。

#include <functional>
#include <optional>
#include <string>

namespace acecode {

enum class DetectedTheme { dark, light };

// 产品入口 — 调真实 I/O。
DetectedTheme detect_terminal_theme();

// 纯函数入口 — 接受注入回调,单测不碰终端。
// osc11_probe:  成功返回 "rgb:RRRR/GGGG/BBBB" 原始响应体,失败/超时返回 nullopt
// env_lookup:   等同 getenv,未设置返回 nullopt
// system_dark:  true=系统暗色模式, false=亮色, nullopt=不可用
DetectedTheme detect_terminal_theme_with(
    const std::function<std::optional<std::string>()>& osc11_probe,
    const std::function<std::optional<std::string>(const char*)>& env_lookup,
    const std::function<std::optional<bool>()>& system_dark_mode);

// 从 OSC 11 响应体 "rgb:RRRR/GGGG/BBBB" 解析并计算 WCAG 相对亮度。
// 成功返回 [0,1] 亮度值,解析失败返回 nullopt。
std::optional<double> parse_osc11_luminance(const std::string& body);

// 从 COLORFGBG 值(如 "0;15" 或 "15;0")判断背景是否为暗色。
// 成功返回 DetectedTheme,解析失败返回 nullopt。
std::optional<DetectedTheme> parse_colorfgbg(const std::string& value);

} // namespace acecode
