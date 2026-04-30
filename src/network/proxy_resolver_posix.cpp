// POSIX 平台 auto_detect 实现。Windows 版本走 proxy_resolver_win.cpp;两个文件
// 相互排斥(`#if !_WIN32` / `#if _WIN32`)。
//
// libcurl 自身已经会读 env,但我们仍然显式解析后写进 ProxyOptions —— 这样:
//   (1) `/proxy` 命令能报告"当前生效代理来自哪个变量",
//   (2) 启动横幅能给用户立即可见的反馈,
//   (3) 跨平台调用方代码完全一致(Windows / POSIX 都走同一个 effective() 流程)。

#if !_WIN32

#include "proxy_resolver.hpp"

#include <cstdlib>
#include <cstring>
#include <string>

namespace acecode::network {

namespace {

// 从 URL 抽 scheme(仅供本文件内部用,头文件没暴露这个 helper)。
std::string scheme_of_url(const std::string& url) {
    auto pos = url.find("://");
    if (pos == std::string::npos) return "https";
    std::string s = url.substr(0, pos);
    for (auto& c : s) c = static_cast<char>(::tolower(static_cast<unsigned char>(c)));
    return s;
}

// 试一组环境变量名,返回第一个非空的 (value, varname)。
struct EnvHit { std::string value; std::string name; };
std::optional<EnvHit> first_env(std::initializer_list<const char*> names) {
    for (const char* n : names) {
        const char* v = std::getenv(n);
        if (v && *v) {
            return EnvHit{std::string(v), std::string(n)};
        }
    }
    return std::nullopt;
}

} // anonymous namespace

ResolvedProxy auto_detect(const NetworkConfig& /*cfg*/, const std::string& target_url) {
    std::string scheme = scheme_of_url(target_url);

    // HTTPS 目标优先走 HTTPS_PROXY,然后 ALL_PROXY,再 fallback HTTP_PROXY。
    // HTTP 目标走 HTTP_PROXY → ALL_PROXY → HTTPS_PROXY(libcurl 风格)。
    std::vector<const char*> ordered;
    if (scheme == "https") {
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
                return {norm, std::string("env:") + n};
            }
        }
    }

    return {"", "none"};
}

} // namespace acecode::network

#endif // !_WIN32
