// Windows 平台 auto_detect 实现。回退链(design.md Decision 1):
//   1) WinHTTP IE config (覆盖 Fiddler/Charles 注入点)
//   2) HKCU 注册表 ProxyEnable/ProxyServer
//   3) 环境变量(与 POSIX 行为对齐)
//
// MSVC 与 MinGW 都通过 CMakeLists 链接 winhttp;此处再加一道 `#pragma` 是为了
// 让 IDE 直接打开本文件 build 时也能解析符号。

#if _WIN32

#include "proxy_resolver.hpp"
#include "../utils/logger.hpp"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winhttp.h>

#if defined(_MSC_VER)
#pragma comment(lib, "winhttp.lib")
#endif

#include <cstdlib>
#include <optional>
#include <string>
#include <vector>

namespace acecode::network {

namespace {

std::string wide_to_utf8(LPCWSTR ws) {
    if (!ws) return "";
    int sz = ::WideCharToMultiByte(CP_UTF8, 0, ws, -1, nullptr, 0, nullptr, nullptr);
    if (sz <= 0) return "";
    std::string out(static_cast<size_t>(sz - 1), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, ws, -1, out.data(), sz, nullptr, nullptr);
    return out;
}

std::string scheme_of_url(const std::string& url) {
    auto pos = url.find("://");
    if (pos == std::string::npos) return "https";
    std::string s = url.substr(0, pos);
    for (auto& c : s) c = static_cast<char>(::tolower(static_cast<unsigned char>(c)));
    return s;
}

// (1) WinHTTP — IE 设置(Fiddler/Charles 修改的就是这一处)。
// 失败 / 未启用 → nullopt。任何 SEH/异常都吞掉避免阻塞启动。
std::optional<std::string> try_winhttp_ie(const std::string& target_scheme) {
    WINHTTP_CURRENT_USER_IE_PROXY_CONFIG cfg = {};
    BOOL ok = FALSE;
    try {
        ok = ::WinHttpGetIEProxyConfigForCurrentUser(&cfg);
    } catch (...) {
        LOG_WARN("[proxy] WinHttpGetIEProxyConfigForCurrentUser threw; falling back");
        return std::nullopt;
    }
    if (!ok) {
        DWORD err = ::GetLastError();
        // ERROR_FILE_NOT_FOUND 表示当前用户从未配置 IE 代理 —— 安静回退。
        if (err != ERROR_FILE_NOT_FOUND) {
            LOG_WARN("[proxy] WinHttpGetIEProxyConfigForCurrentUser failed: " +
                     std::to_string(err));
        }
        return std::nullopt;
    }

    auto cleanup = [&]() {
        if (cfg.lpszAutoConfigUrl) ::GlobalFree(cfg.lpszAutoConfigUrl);
        if (cfg.lpszProxy) ::GlobalFree(cfg.lpszProxy);
        if (cfg.lpszProxyBypass) ::GlobalFree(cfg.lpszProxyBypass);
    };

    if (!cfg.lpszProxy || !*cfg.lpszProxy) {
        cleanup();
        return std::nullopt;
    }

    std::string raw = wide_to_utf8(cfg.lpszProxy);
    cleanup();

    std::string parsed = parse_winhttp_proxy_string(raw, target_scheme);
    if (parsed.empty()) return std::nullopt;
    return parsed;
}

// (2) 注册表回退。WinHTTP API 在某些精简 SKU 上不可用时仍然能拿到。
std::optional<std::string> try_registry(const std::string& target_scheme) {
    HKEY hk = nullptr;
    LONG rc = ::RegOpenKeyExW(HKEY_CURRENT_USER,
                              L"Software\\Microsoft\\Windows\\CurrentVersion\\Internet Settings",
                              0, KEY_READ, &hk);
    if (rc != ERROR_SUCCESS) {
        LOG_WARN("[proxy] RegOpenKeyExW Internet Settings failed: " + std::to_string(rc));
        return std::nullopt;
    }

    DWORD enabled = 0;
    DWORD enabled_size = sizeof(enabled);
    rc = ::RegQueryValueExW(hk, L"ProxyEnable", nullptr, nullptr,
                            reinterpret_cast<LPBYTE>(&enabled), &enabled_size);
    if (rc != ERROR_SUCCESS || enabled == 0) {
        ::RegCloseKey(hk);
        return std::nullopt;
    }

    DWORD type = 0;
    DWORD bytes = 0;
    rc = ::RegQueryValueExW(hk, L"ProxyServer", nullptr, &type, nullptr, &bytes);
    if (rc != ERROR_SUCCESS || type != REG_SZ || bytes == 0) {
        ::RegCloseKey(hk);
        return std::nullopt;
    }
    std::wstring buf(bytes / sizeof(wchar_t), L'\0');
    rc = ::RegQueryValueExW(hk, L"ProxyServer", nullptr, nullptr,
                            reinterpret_cast<LPBYTE>(buf.data()), &bytes);
    ::RegCloseKey(hk);
    if (rc != ERROR_SUCCESS) return std::nullopt;

    while (!buf.empty() && buf.back() == L'\0') buf.pop_back();
    std::string raw = wide_to_utf8(buf.c_str());
    std::string parsed = parse_winhttp_proxy_string(raw, target_scheme);
    if (parsed.empty()) return std::nullopt;
    return parsed;
}

// (3) 环境变量。Windows 也会有用户在 cmd / PowerShell 设置 HTTPS_PROXY,与
// POSIX 行为保持一致。
std::optional<std::pair<std::string, std::string>> try_env(const std::string& target_scheme) {
    std::vector<const char*> ordered;
    if (target_scheme == "https") {
        ordered = {"HTTPS_PROXY", "https_proxy", "ALL_PROXY", "all_proxy",
                   "HTTP_PROXY", "http_proxy"};
    } else {
        ordered = {"HTTP_PROXY", "http_proxy", "ALL_PROXY", "all_proxy",
                   "HTTPS_PROXY", "https_proxy"};
    }
    for (const char* n : ordered) {
        const char* v = std::getenv(n);
        if (v && *v) {
            std::string norm = normalize_proxy_url(v);
            if (!norm.empty()) {
                return std::make_pair(norm, std::string("env:") + n);
            }
        }
    }
    return std::nullopt;
}

} // anonymous namespace

ResolvedProxy auto_detect(const NetworkConfig& /*cfg*/, const std::string& target_url) {
    std::string scheme = scheme_of_url(target_url);

    if (auto v = try_winhttp_ie(scheme); v.has_value() && !v->empty()) {
        return {*v, "windows-system"};
    }
    if (auto v = try_registry(scheme); v.has_value() && !v->empty()) {
        return {*v, "windows-registry"};
    }
    if (auto e = try_env(scheme); e.has_value()) {
        return {e->first, e->second};
    }
    return {"", "none"};
}

} // namespace acecode::network

#endif // _WIN32
