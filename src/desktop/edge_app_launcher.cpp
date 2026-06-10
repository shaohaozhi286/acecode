#include "edge_app_launcher.hpp"

#include "../config/config.hpp"
#include "../utils/encoding.hpp"
#include "../utils/logger.hpp"
#include "../utils/utf8_path.hpp"

#include <sstream>
#include <system_error>
#include <utility>

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#  include <knownfolders.h>
#  include <shlobj.h>
#  include <shellapi.h>
#endif

namespace fs = std::filesystem;

namespace acecode::desktop {

namespace {

constexpr const wchar_t* kMsEdgeExeName = L"msedge.exe";

fs::path edge_application_exe(const fs::path& root) {
    return root / "Microsoft" / "Edge" / "Application" / "msedge.exe";
}

std::wstring quote_arg_w(const std::wstring& s) {
    const bool need_quotes = s.empty() || s.find_first_of(L" \t\"") != std::wstring::npos;
    if (!need_quotes) return s;

    std::wstring out;
    out.reserve(s.size() + 2);
    out.push_back(L'"');
    int backslashes = 0;
    for (wchar_t c : s) {
        if (c == L'\\') {
            ++backslashes;
        } else if (c == L'"') {
            for (int i = 0; i < backslashes * 2 + 1; ++i) out.push_back(L'\\');
            out.push_back(L'"');
            backslashes = 0;
        } else {
            for (int i = 0; i < backslashes; ++i) out.push_back(L'\\');
            backslashes = 0;
            out.push_back(c);
        }
    }
    for (int i = 0; i < backslashes * 2; ++i) out.push_back(L'\\');
    out.push_back(L'"');
    return out;
}

#ifdef _WIN32
std::string windows_error_suffix(DWORD err) {
    std::ostringstream oss;
    oss << " (gle=" << err << ")";
    return oss.str();
}

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

fs::path edge_user_data_dir_root() {
    return acecode::path_from_utf8(acecode::get_acecode_dir()) / "edge-app-profile";
}

// 准备本次启动专用的干净 user-data-dir:
//   1. 在 root 下用 PID 命名一个子目录(edge_profile_subdir_name);
//   2. best-effort 清掉 root 下所有旧子目录 —— 上一轮崩溃残留的、当前没有 msedge
//      占用的会被删掉;仍被某个残留 msedge 锁着的删不掉也无所谓(我们用的是全新的
//      PID 子目录,残留进程影响不到本次启动);
//   3. 创建本次子目录。
// 返回空 path 表示连本次子目录都建不出来(磁盘满 / 权限),调用方据此报错。
fs::path prepare_clean_user_data_dir() {
    fs::path root = edge_user_data_dir_root();
    std::error_code ec;
    fs::create_directories(root, ec);

    // 先收集再删除,避免边迭代边删在 Windows 上的未定义行为。
    std::vector<fs::path> stale;
    {
        std::error_code it_ec;
        fs::directory_iterator it(root, fs::directory_options::skip_permission_denied, it_ec);
        if (!it_ec) {
            for (const auto& entry : it) {
                std::error_code e2;
                if (entry.is_directory(e2) && !e2) stale.push_back(entry.path());
            }
        }
    }
    for (const auto& dir : stale) {
        std::error_code rm_ec;
        fs::remove_all(dir, rm_ec); // 锁着的删不掉,忽略
    }

    fs::path mine = root / acecode::path_from_utf8(
                               edge_profile_subdir_name(::GetCurrentProcessId()));
    std::error_code mk_ec;
    fs::create_directories(mine, mk_ec);
    if (mk_ec) {
        LOG_WARN("[desktop] failed to create Edge user data dir: " + mk_ec.message());
        return {};
    }
    return mine;
}
#endif

} // namespace

std::string edge_profile_subdir_name(unsigned long pid) {
    // "u" 前缀 + 十进制 PID。前缀避免纯数字目录名在某些工具里被误解析,也便于
    // 一眼看出是 acecode 造的 per-launch profile。
    return "u" + std::to_string(pid);
}

std::optional<fs::path> find_msedge_executable_in(const std::vector<fs::path>& roots) {
    for (const auto& root : roots) {
        if (root.empty()) continue;
        fs::path candidate = edge_application_exe(root);
        std::error_code ec;
        if (fs::is_regular_file(candidate, ec) && !ec) {
            return candidate;
        }
    }
    return std::nullopt;
}

std::wstring build_edge_app_parameters_w(const std::wstring& url,
                                         const std::wstring& user_data_dir) {
    std::vector<std::wstring> argv;
    argv.push_back(L"--app=" + url);
    argv.push_back(L"--user-data-dir=" + user_data_dir);
    argv.push_back(L"--no-first-run");

    std::wostringstream oss;
    for (std::size_t i = 0; i < argv.size(); ++i) {
        if (i) oss << L' ';
        oss << quote_arg_w(argv[i]);
    }
    return oss.str();
}

#ifdef _WIN32
std::optional<fs::path> find_msedge_executable() {
    std::vector<fs::path> roots;
    if (auto pf = known_folder_path(FOLDERID_ProgramFiles); !pf.empty()) {
        roots.push_back(std::move(pf));
    }
    if (auto pfx86 = known_folder_path(FOLDERID_ProgramFilesX86); !pfx86.empty()) {
        roots.push_back(std::move(pfx86));
    }
    if (auto local = known_folder_path(FOLDERID_LocalAppData); !local.empty()) {
        roots.push_back(std::move(local));
    }
    return find_msedge_executable_in(roots);
}

EdgeAppLaunchHandle launch_edge_app(const std::string& url) {
    EdgeAppLaunchHandle result;
    if (url.empty() || url == "about:blank") {
        result.error = "daemon URL is unavailable";
        return result;
    }

    std::wstring url_w = acecode::utf8_to_wide(url);
    if (url_w.empty()) {
        result.error = "daemon URL cannot be converted to UTF-16";
        return result;
    }

    fs::path user_data_dir = prepare_clean_user_data_dir();
    if (user_data_dir.empty()) {
        result.error = "failed to prepare Edge user data dir";
        return result;
    }

    std::wstring file_w = kMsEdgeExeName;
    if (auto exe = find_msedge_executable(); exe.has_value()) {
        file_w = exe->wstring();
        LOG_INFO("[desktop] Edge app fallback using " + acecode::path_to_utf8(*exe));
    } else {
        LOG_WARN("[desktop] msedge.exe not found under known install roots; "
                 "falling back to ShellExecute app resolution");
    }

    std::wstring params = build_edge_app_parameters_w(url_w, user_data_dir.wstring());

    SHELLEXECUTEINFOW sei{};
    sei.cbSize = sizeof(sei);
    sei.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_FLAG_NO_UI;
    sei.lpVerb = L"open";
    sei.lpFile = file_w.c_str();
    sei.lpParameters = params.c_str();
    sei.nShow = SW_SHOWNORMAL;

    if (!::ShellExecuteExW(&sei)) {
        DWORD err = ::GetLastError();
        result.error = "ShellExecuteExW(msedge.exe) failed" + windows_error_suffix(err);
        return result;
    }
    if (!sei.hProcess) {
        result.error = "ShellExecuteExW(msedge.exe) returned no process handle";
        return result;
    }

    result.ok = true;
    result.process = sei.hProcess;
    result.pid = ::GetProcessId(sei.hProcess);
    LOG_INFO("[desktop] Edge app fallback launched (pid=" + std::to_string(result.pid) + ")");
    return result;
}

EdgeAppLaunchResult launch_edge_app_and_wait(const std::string& url) {
    EdgeAppLaunchResult result;
    auto launched = launch_edge_app(url);
    if (!launched.ok) {
        result.error = launched.error;
        return result;
    }

    HANDLE process = static_cast<HANDLE>(launched.process);
    LOG_INFO("[desktop] Edge app fallback launched; waiting for app process to exit");
    ::WaitForSingleObject(process, INFINITE);
    DWORD exit_code = 0;
    if (::GetExitCodeProcess(process, &exit_code)) {
        result.exit_code = static_cast<unsigned long>(exit_code);
    }
    ::CloseHandle(process);
    result.ok = true;
    return result;
}

#else

std::optional<fs::path> find_msedge_executable() {
    return std::nullopt;
}

EdgeAppLaunchHandle launch_edge_app(const std::string& /*url*/) {
    EdgeAppLaunchHandle result;
    result.error = "Edge app fallback is only supported on Windows";
    return result;
}

EdgeAppLaunchResult launch_edge_app_and_wait(const std::string& /*url*/) {
    EdgeAppLaunchResult result;
    result.error = "Edge app fallback is only supported on Windows";
    return result;
}

#endif

} // namespace acecode::desktop
