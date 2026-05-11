#pragma once

// WebView2 Runtime 探测 — 给 acecode-desktop 在系统未安装独立 WebView2
// Evergreen Runtime 时找一条 fallback。
//
// 背景:许多企业 / 信创环境的机器只装了 Microsoft Edge 浏览器,而没有单独
// 安装 "Microsoft Edge WebView2 Runtime"。第三方 WebView2 应用(包括
// acecode-desktop)默认走 WebView2Loader.dll 的 Evergreen Runtime 发现
// 路径,这条路径不会去看 Edge 浏览器目录,导致 CreateCoreWebView2-
// EnvironmentWithOptions 失败。绝大多数 Edge 100+ 版本的浏览器目录里也
// 自带 msedgewebview2.exe + 同版本 dll,可以直接通过把
// WEBVIEW2_BROWSER_EXECUTABLE_FOLDER 环境变量指过去复用,等价于
// CreateCoreWebView2EnvironmentWithOptions 的 browserExecutableFolder
// 参数(WebView2Loader.dll 加载时会优先读这个 env)。
//
// 调用时机:WebHost 第一次构造 webview::webview 失败时(catch 里),不要
// 在启动早期主动 set,以免覆盖正常装了 Evergreen Runtime 的用户路径。
//
// 平台:Windows-only;POSIX 上头文件保持可见,但实现返回空(macOS / Linux
// 走 WKWebView / WebKitGTK,跟 WebView2 无关)。

#include <filesystem>
#include <optional>
#include <vector>

namespace acecode::desktop {

// 在给定的根目录列表中查找最新版本的 Edge 浏览器自带 msedgewebview2.exe
// 所在文件夹。纯函数,不调系统 API,unit test 喂临时目录覆盖。
//
// 期望的目录结构(Edge 浏览器标准布局):
//   <root>/Microsoft/Edge/Application/<version>/msedgewebview2.exe
// version 必须是 4 段数字格式 "<n>.<n>.<n>.<n>"(过滤 "SetupMetrics" /
// "Installer" / 回滚备份目录等非版本 entry)。
//
// 返回:命中时 = 包含 msedgewebview2.exe 的那个 <version> 目录绝对路径。
//      未命中 = std::nullopt。
//
// 排序:存在多个版本子目录时,按 4 段版本号数值字典序选最大版本。
// 多个 root 都命中时,**返回所有命中里版本号最大的那个**(不是按 root 顺序),
// 这样 PF 与 PFx86 都装了 Edge 的机器拿到最新一份。
std::optional<std::filesystem::path> find_edge_browser_folder_in(
    const std::vector<std::filesystem::path>& roots);

// Windows 系统调用版:用 SHGetKnownFolderPath 拿 ProgramFiles 与
// ProgramFiles(x86) 两个根,调上面的纯函数。POSIX 上始终返回 std::nullopt。
//
// 失败语义和 find_edge_browser_folder_in 一致:返回 nullopt 表示当前机器
// 没有可复用的 Edge 浏览器二进制,调用方应当向用户提示安装 WebView2 Runtime。
std::optional<std::filesystem::path> find_edge_browser_folder();

} // namespace acecode::desktop
