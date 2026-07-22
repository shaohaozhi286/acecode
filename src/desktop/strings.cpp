#include "strings.hpp"

#include "locale.hpp"

#include <array>
#include <mutex>

namespace acecode::desktop {

namespace {

using Catalog = std::array<std::string_view,
                           static_cast<std::size_t>(DesktopStringId::Count)>;

constexpr Catalog kZhCn = {
    "未知",
    "已置顶",
    "最近",
    "更多",
    "新建会话",
    "打开 ACECode",
    "退出",
    "新会话 1",
    "关于 ACECode",
    "ACECode 版本：",
    "浏览器版本：",
    "编译器版本：",
    "选择项目文件夹",
    "选择",
    "未找到目录选择工具：请安装 zenity（如 sudo apt install zenity）或 kdialog 后重试",
    "ACECode 启动失败",
    "ACECode 桌面版启动时遇到未预期的错误，即将退出。",
    "请把日志文件 %USERPROFILE%\\.acecode\\logs\\desktop-*.log 发给 IT/开发团队。",
    "异常信息：",
    "无法找到 acecode.exe。请确认它与 acecode-desktop.exe 位于同一目录。",
    "无法为工作区启动后台服务",
    "ACECode 无法启动内置 WebView，且后台服务也未能就绪，无法回退到浏览器。",
    "ACECode 无法启动内置 WebView，回退到浏览器也失败了。",
    "原因：",
    "WebView2 失败：",
    "浏览器失败：",
    "已完成 · ",
    "会话",
    "（空白回合）",
};

constexpr Catalog kEnUs = {
    "Unknown",
    "Pinned",
    "Recent",
    "More",
    "New session",
    "Open ACECode",
    "Quit",
    "New session 1",
    "About ACECode",
    "ACECode version: ",
    "Browser version: ",
    "Compiler version: ",
    "Select project folder",
    "Select",
    "No folder picker was found. Install zenity (for example, sudo apt install zenity) or kdialog and try again.",
    "ACECode startup failed",
    "ACECode Desktop encountered an unexpected startup error and will exit.",
    "Send %USERPROFILE%\\.acecode\\logs\\desktop-*.log to your IT or development team.",
    "Exception details: ",
    "Cannot locate acecode.exe. Place it in the same directory as acecode-desktop.exe.",
    "Failed to start the background service for workspace",
    "ACECode could not start the embedded WebView, and the background service is not ready, so browser fallback is unavailable.",
    "ACECode could not start the embedded WebView, and browser fallback also failed.",
    "Reason: ",
    "WebView2 failure: ",
    "Browser failure: ",
    "Completed · ",
    "Session",
    "(blank turn)",
};

static_assert(kZhCn.size() == kEnUs.size());

std::mutex g_locale_mu;
std::string g_locale = kLocaleZhCn;

const Catalog& catalog_for(const std::string& locale) {
    return locale == kLocaleEnUs ? kEnUs : kZhCn;
}

void append_labeled_detail(std::string& out,
                           DesktopStringId label,
                           const std::string& detail,
                           const std::string& locale) {
    if (detail.empty()) return;
    out += "\n\n";
    out += desktop_string(label, locale);
    out += "\n";
    out += detail;
}

} // namespace

std::string_view desktop_string(DesktopStringId id, const std::string& locale) {
    const auto index = static_cast<std::size_t>(id);
    if (index >= static_cast<std::size_t>(DesktopStringId::Count)) return {};
    return catalog_for(locale)[index];
}

void set_native_locale(const std::string& locale) {
    std::lock_guard<std::mutex> lock(g_locale_mu);
    g_locale = is_supported_locale(locale) ? locale : kLocaleZhCn;
}

std::string native_locale() {
    std::lock_guard<std::mutex> lock(g_locale_mu);
    return g_locale;
}

std::string_view native_string(DesktopStringId id) {
    // Catalog entries have static storage; the copied locale only chooses one.
    return desktop_string(id, native_locale());
}

std::string format_startup_exception_message(const std::string& details,
                                             const std::string& locale) {
    std::string out(desktop_string(DesktopStringId::StartupUnexpected, locale));
    out += "\n\n";
    out += desktop_string(DesktopStringId::StartupLogInstruction, locale);
    if (!details.empty()) {
        out += "\n\n";
        out += desktop_string(DesktopStringId::StartupException, locale);
        out += "\n";
        out += details;
    }
    return out;
}

std::string format_daemon_missing_message(const std::string& locale) {
    return std::string(desktop_string(DesktopStringId::DaemonMissing, locale));
}

std::string format_daemon_workspace_failed_message(const std::string& workspace,
                                                   const std::string& details,
                                                   const std::string& locale) {
    std::string out(desktop_string(DesktopStringId::DaemonWorkspaceFailed, locale));
    if (!workspace.empty()) out += " '" + workspace + "'";
    if (!details.empty()) out += ":\n" + details;
    return out;
}

std::string format_browser_fallback_no_daemon_message(
    const std::string& reason,
    const std::string& webview_error,
    const std::string& locale) {
    std::string out(desktop_string(DesktopStringId::BrowserFallbackNoDaemon, locale));
    append_labeled_detail(out, DesktopStringId::ReasonLabel, reason, locale);
    append_labeled_detail(out, DesktopStringId::WebViewFailureLabel,
                          webview_error, locale);
    return out;
}

std::string format_browser_fallback_open_failed_message(
    const std::string& reason,
    const std::string& browser_error,
    const std::string& webview_error,
    const std::string& locale) {
    std::string out(desktop_string(DesktopStringId::BrowserFallbackOpenFailed, locale));
    append_labeled_detail(out, DesktopStringId::ReasonLabel, reason, locale);
    append_labeled_detail(out, DesktopStringId::BrowserFailureLabel,
                          browser_error, locale);
    append_labeled_detail(out, DesktopStringId::WebViewFailureLabel,
                          webview_error, locale);
    return out;
}

} // namespace acecode::desktop
