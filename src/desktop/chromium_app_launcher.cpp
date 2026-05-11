// chromium_app_launcher 实现。设计 + 调用时机见 chromium_app_launcher.hpp。

#include "chromium_app_launcher.hpp"

#include "../utils/encoding.hpp"
#include "../utils/logger.hpp"

#include <array>
#include <string>
#include <system_error>

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#  include <shlobj.h>
#  include <knownfolders.h>
#endif

namespace fs = std::filesystem;

namespace acecode::desktop {

namespace {

// 候选 Chromium 系浏览器,(<root>/<relative-from-root>, display_name)。Edge
// 优先 — 同事场景里 Edge 几乎必装,且 Edge UI 跟我们想要的 "原生 app" 体感
// 最接近。Chrome 次之。其它二线 Chromium(360/QQ/Brave/Vivaldi)有的不兼容
// --app= 或参数改了名,本期不试,留给 ShellExecuteW 兜底默认浏览器。
constexpr std::array<std::pair<const char*, const char*>, 2> kBrowserCandidates = {{
    {"Microsoft/Edge/Application/msedge.exe",     "Microsoft Edge"},
    {"Google/Chrome/Application/chrome.exe",      "Google Chrome"},
}};

bool path_exists_file(const fs::path& p) {
    std::error_code ec;
    return fs::is_regular_file(p, ec) && !ec;
}

} // namespace

std::optional<ChromiumBrowser> find_chromium_app_browser_in(
    const std::vector<fs::path>& roots) {
    // 先按候选浏览器顺序遍历,再按 roots 顺序;这样 Edge 在任一 root 命中
    // 都优先于 Chrome。
    for (const auto& [rel_path, display_name] : kBrowserCandidates) {
        for (const auto& root : roots) {
            if (root.empty()) continue;
            fs::path candidate = root / rel_path;
            if (path_exists_file(candidate)) {
                return ChromiumBrowser{candidate, display_name};
            }
        }
    }
    return std::nullopt;
}

#ifdef _WIN32

namespace {

fs::path known_folder_path(REFKNOWNFOLDERID id) {
    PWSTR raw = nullptr;
    HRESULT hr = ::SHGetKnownFolderPath(id, KF_FLAG_DEFAULT, nullptr, &raw);
    if (FAILED(hr) || !raw) {
        if (raw) ::CoTaskMemFree(raw);
        return {};
    }
    fs::path result(raw);
    ::CoTaskMemFree(raw);
    return result;
}

} // namespace

std::optional<ChromiumBrowser> find_chromium_app_browser() {
    std::vector<fs::path> roots;
    if (auto pf = known_folder_path(FOLDERID_ProgramFiles); !pf.empty()) {
        roots.push_back(std::move(pf));
    }
    if (auto pfx86 = known_folder_path(FOLDERID_ProgramFilesX86); !pfx86.empty()) {
        roots.push_back(std::move(pfx86));
    }
    if (roots.empty()) {
        LOG_WARN("[chromium_app] SHGetKnownFolderPath returned no ProgramFiles paths");
        return std::nullopt;
    }
    return find_chromium_app_browser_in(roots);
}

bool launch_chromium_app_mode(const fs::path& exe,
                              const std::string& url,
                              std::string& error) {
    // 拼 command line:CreateProcessW 的 lpCommandLine 必须可写,且第一个
    // 实参约定是程序名(被 argv[0] 拿)。带空格的路径要加引号。
    //   "<exe>" --app="<url>"
    std::wstring cmd;
    cmd.reserve(exe.wstring().size() + url.size() + 16);
    cmd.push_back(L'"');
    cmd += exe.wstring();
    cmd.push_back(L'"');
    cmd += L" --app=\"";
    cmd += acecode::utf8_to_wide(url);
    cmd.push_back(L'"');

    // lpCommandLine 在 CreateProcessW 路径下可能被内部修改(strtok 风格),
    // 必须传非 const 缓冲区。std::wstring::data() 自 C++17 起返回可写指针。
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};

    // 不传 lpApplicationName 而是让 CreateProcessW 从 cmd 第一个 token 解析,
    // 这样 Chrome / Edge 自己也能依赖 argv[0] 正确推断安装目录。
    // 不带 CREATE_SUSPENDED / CREATE_BREAKAWAY_FROM_JOB —— 浏览器进程加入
    // desktop 的 Job 也无所谓(浏览器自己创建子进程时会用
    // PROCESS_BREAKAWAY,绕开 KILL_ON_JOB_CLOSE);desktop 退出时浏览器跟着
    // kill 也是预期(用户从托盘 quit 时,浏览器开着没意义)。
    BOOL ok = ::CreateProcessW(
        /*lpApplicationName=*/nullptr,
        /*lpCommandLine=*/cmd.data(),
        /*lpProcessAttributes=*/nullptr,
        /*lpThreadAttributes=*/nullptr,
        /*bInheritHandles=*/FALSE,
        /*dwCreationFlags=*/0,
        /*lpEnvironment=*/nullptr,
        /*lpCurrentDirectory=*/nullptr,
        &si, &pi);
    if (!ok) {
        const DWORD gle = ::GetLastError();
        error = "CreateProcessW failed, gle=" + std::to_string(gle);
        return false;
    }
    // 父进程不等待子进程结束,直接关掉句柄。
    ::CloseHandle(pi.hThread);
    ::CloseHandle(pi.hProcess);
    return true;
}

#else // _WIN32

std::optional<ChromiumBrowser> find_chromium_app_browser() {
    return std::nullopt;
}

bool launch_chromium_app_mode(const fs::path& /*exe*/,
                              const std::string& /*url*/,
                              std::string& error) {
    error = "platform not supported";
    return false;
}

#endif // _WIN32

} // namespace acecode::desktop
