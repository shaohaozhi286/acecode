#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace acecode::desktop {

// 窗口打底色(不透明 RGB)。快速 resize 时 WebView2 合成器跟不上,新暴露区域
// 由宿主窗口的类背景刷先打底 — 这个颜色应当与前端 body 底色(--ace-bg)一致,
// 否则用户会看到黑/白闪边(webview 类应用通病,Electron/Tauri 同样按此处理)。
struct WindowBackgroundColor {
    std::uint8_t r = 0;
    std::uint8_t g = 0;
    std::uint8_t b = 0;

    bool operator==(const WindowBackgroundColor& other) const {
        return r == other.r && g == other.g && b == other.b;
    }
};

// 启动默认打底色 = 前端浅色主题 body 底色(globals.css --ace-bg #f5f5f2)。
// 前端 mount 后会经 aceDesktop_setWindowBackgroundColor 推送真实主题色覆盖。
inline constexpr WindowBackgroundColor kDefaultWindowBackground{0xF5, 0xF5, 0xF2};

// 把 JS 端传的颜色字符串解析成不透明 RGB。只接受 "#RRGGBB" / "RRGGBB"
// (十六进制,大小写均可)。其它形态(空串 / #RGB 短形 / 8 位带 alpha /
// rgb(...) / 含非 hex 字符 / 前后空白)一律 nullopt — bridge 入口是哨兵,
// 与 parse_resize_direction 同款策略:非法输入拒绝而不是猜,防止前端笔误
// 把窗口涂成意外颜色。规范化(trim / #RGB 展开)由前端纯函数负责。
std::optional<WindowBackgroundColor> parse_window_background_color(std::string_view text);

// WEBVIEW2_DEFAULT_BACKGROUND_COLOR 环境变量取值。WebView2 官方文档:值是
// 8 位 hex 且前两位是 alpha,不足 8 位会被前补 00 解释成全透明 — 所以必须
// 恒输出 "FF" 开头的完整 8 位(如 {F5,F5,F2} → "FFF5F5F2")。
std::string webview2_default_background_env_value(WindowBackgroundColor color);

} // namespace acecode::desktop
