// WebView2 Runtime 探测实现。设计 + 调用时机见 webview2_runtime_probe.hpp。

#include "webview2_runtime_probe.hpp"

#include "../utils/logger.hpp"

#include <array>
#include <cstdint>
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

constexpr const char* kEdgeWebViewExecutableName = "msedgewebview2.exe";

// 把版本号字符串("100.0.1234.56")拆 4 段非负整数。失败返回 std::nullopt
// (整段不是 4 位、含非数字字符、任何一段超过 uint32 上限都视为非版本)。
std::optional<std::array<std::uint32_t, 4>> parse_version_4(const std::string& s) {
    std::array<std::uint32_t, 4> out{0, 0, 0, 0};
    std::size_t segment = 0;
    std::uint64_t acc = 0;
    bool any_digit_in_segment = false;
    for (std::size_t i = 0; i <= s.size(); ++i) {
        const bool at_end = (i == s.size());
        const char c = at_end ? '.' : s[i];
        if (c == '.') {
            if (!any_digit_in_segment) return std::nullopt;
            if (segment >= 4) return std::nullopt;
            if (acc > 0xFFFFFFFFu) return std::nullopt;
            out[segment] = static_cast<std::uint32_t>(acc);
            ++segment;
            acc = 0;
            any_digit_in_segment = false;
            continue;
        }
        if (c < '0' || c > '9') return std::nullopt;
        acc = acc * 10 + static_cast<std::uint64_t>(c - '0');
        if (acc > 0xFFFFFFFFu) return std::nullopt;
        any_digit_in_segment = true;
    }
    if (segment != 4) return std::nullopt;
    return out;
}

// Edge 浏览器标准布局 <root>/Microsoft/Edge/Application/。我们只用这一条
// 路径 — Edge Beta/Dev/Canary 各有自己的 root,但同事场景下用户装的就是
// 正式版 Edge,本期不扫 Beta/Dev/Canary,避免误用 Canary 这种快速变化通道。
fs::path edge_application_dir(const fs::path& root) {
    return root / "Microsoft" / "Edge" / "Application";
}

// 在一个 Application 目录下扫所有合法版本子目录,挑最大版本 + 验证
// msedgewebview2.exe 存在。返回 (version, folder),无候选返回 nullopt。
std::optional<std::pair<std::array<std::uint32_t, 4>, fs::path>>
pick_latest_in_application_dir(const fs::path& application_dir) {
    std::error_code ec;
    if (!fs::is_directory(application_dir, ec) || ec) {
        return std::nullopt;
    }

    std::optional<std::array<std::uint32_t, 4>> best_version;
    fs::path best_folder;

    fs::directory_iterator it(application_dir, fs::directory_options::skip_permission_denied, ec);
    if (ec) return std::nullopt;

    for (const auto& entry : it) {
        std::error_code ec_entry;
        if (!entry.is_directory(ec_entry) || ec_entry) continue;

        const std::string name = entry.path().filename().string();
        auto parsed = parse_version_4(name);
        if (!parsed.has_value()) continue;

        const fs::path candidate_exe = entry.path() / kEdgeWebViewExecutableName;
        std::error_code exe_ec;
        if (!fs::is_regular_file(candidate_exe, exe_ec) || exe_ec) {
            // Edge 浏览器某些 channel 切换时会保留只剩 Resources 的旧版本号
            // 目录,但没了 msedgewebview2.exe — 直接跳过,免得后面 set env
            // 指向死路径。
            continue;
        }

        if (!best_version.has_value() || *parsed > *best_version) {
            best_version = parsed;
            best_folder = entry.path();
        }
    }

    if (!best_version.has_value()) return std::nullopt;
    return std::make_pair(*best_version, best_folder);
}

} // namespace

std::optional<fs::path> find_edge_browser_folder_in(
    const std::vector<fs::path>& roots) {
    std::optional<std::array<std::uint32_t, 4>> best_version;
    fs::path best_folder;

    for (const auto& root : roots) {
        if (root.empty()) continue;
        auto pick = pick_latest_in_application_dir(edge_application_dir(root));
        if (!pick.has_value()) continue;

        if (!best_version.has_value() || pick->first > *best_version) {
            best_version = pick->first;
            best_folder = pick->second;
        }
    }

    if (!best_version.has_value()) return std::nullopt;
    return best_folder;
}

#ifdef _WIN32
namespace {

// SHGetKnownFolderPath 拿 KNOWNFOLDERID 对应路径。失败返回空 path。COM
// 不需要 init —— SHGetKnownFolderPath 走的是 NTDLL 路径不依赖 apartment。
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

std::optional<fs::path> find_edge_browser_folder() {
    std::vector<fs::path> roots;
    if (auto pf = known_folder_path(FOLDERID_ProgramFiles); !pf.empty()) {
        roots.push_back(std::move(pf));
    }
    if (auto pfx86 = known_folder_path(FOLDERID_ProgramFilesX86); !pfx86.empty()) {
        roots.push_back(std::move(pfx86));
    }
    if (roots.empty()) {
        LOG_WARN("[webview2_probe] SHGetKnownFolderPath returned no ProgramFiles paths");
        return std::nullopt;
    }
    return find_edge_browser_folder_in(roots);
}

#else // _WIN32

std::optional<fs::path> find_edge_browser_folder() {
    return std::nullopt;
}

#endif // _WIN32

} // namespace acecode::desktop
