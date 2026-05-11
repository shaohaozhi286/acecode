#pragma once

// "Chromium app 模式" 浏览器启动器 — 把已经在跑的 daemon URL 喂给一个无地址
// 栏 / 无标签栏 / 无书签栏的浏览器窗口,让用户体感是一个独立 native app,
// 而不是"被丢回浏览器"。所有 Chromium 系浏览器(Edge / Chrome / Brave 等)
// 都支持 `--app=<url>` 参数。
//
// 使用场景:WebView2 不可用(企业 / 信创机器,Edge >=126 不允许第三方借用)
// 时,acecode-desktop 进入"浏览器降级"流程,优先尝试 app 模式 — 用户感知最
// 接近原生 webview;失败再 fallback 到 ShellExecuteW 系统默认浏览器(可能
// 是 IE / Firefox 等非 Chromium,只能用普通 tab)。
//
// 平台:Windows-only。POSIX 上头文件保持可见但实现是 stub(macOS / Linux
// 桌面壳本期未对 WebView2 / WKWebView 失败做浏览器降级)。

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace acecode::desktop {

struct ChromiumBrowser {
    std::filesystem::path exe;     // 浏览器主进程的绝对路径
    std::string display_name;      // 用于日志的可读名(Edge / Chrome / ...)
};

// 在给定根目录列表中查找 Chromium 系浏览器主可执行。纯函数,unit test
// 喂临时目录覆盖。返回首个命中(按 candidates_per_root 的优先级顺序)。
//
// 期望路径形态:<root>/<vendor>/<product>/Application/<exe>。我们只列业内
// 主流的 Edge + Chrome 两条 — 二线 Chromium 系(360/QQ/Brave/...)有的不
// 兼容 --app= 或参数被改名,贸然走会出现"浏览器开了但啥也没显示"的诡异
// 体验,不如直接 fallback 给 ShellExecuteW 让用户看到原始默认浏览器。
std::optional<ChromiumBrowser> find_chromium_app_browser_in(
    const std::vector<std::filesystem::path>& roots);

// Windows 系统调用版:SHGetKnownFolderPath 拿 ProgramFiles / ProgramFiles(x86),
// 调上面纯函数。POSIX stub。
std::optional<ChromiumBrowser> find_chromium_app_browser();

// 启动 browser.exe 用 --app=url 把窗口拉起来,detached 模式(不阻塞、不
// 加入 desktop 的 Job Object — 浏览器是用户进程,desktop 退出时不该跟着
// kill,虽然 daemon 会因 Job 关闭被 kill 让浏览器加载失败,但这是用户从
// 托盘 quit 的预期行为)。
//
// 返回 true = CreateProcessW 成功(进程已 spawn,不保证窗口已渲染);
// false + error 字符串 = CreateProcessW 失败,调用方应当 fallback。POSIX
// 上始终返 false / "platform not supported"。
bool launch_chromium_app_mode(const std::filesystem::path& exe,
                              const std::string& url,
                              std::string& error);

} // namespace acecode::desktop
