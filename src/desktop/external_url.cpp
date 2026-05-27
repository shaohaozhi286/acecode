#include "external_url.hpp"

#include "../utils/encoding.hpp"

#include <algorithm>
#include <cctype>

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#  include <shellapi.h>
#else
#  include <unistd.h>
#endif

namespace acecode::desktop {
namespace {

std::string lower_ascii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

bool platform_open_external_url(const std::string& url, std::string& error) {
#ifdef _WIN32
    const std::wstring wide_url = acecode::utf8_to_wide(url);
    HINSTANCE result = ::ShellExecuteW(
        nullptr, L"open", wide_url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    auto code = reinterpret_cast<intptr_t>(result);
    if (code > 32) return true;
    error = "ShellExecute failed: " + std::to_string(code);
    return false;
#else
#  ifdef __APPLE__
    const char* opener = "open";
#  else
    const char* opener = "xdg-open";
#  endif
    pid_t pid = ::fork();
    if (pid < 0) {
        error = "fork failed";
        return false;
    }
    if (pid == 0) {
        ::execlp(opener, opener, url.c_str(), static_cast<char*>(nullptr));
        ::_exit(127);
    }
    return true;
#endif
}

} // namespace

bool is_safe_external_url(const std::string& url) {
    if (url.empty()) return false;
    const auto colon = url.find(':');
    if (colon == std::string::npos) return false;
    const std::string scheme = lower_ascii(url.substr(0, colon));
    if (scheme != "http" && scheme != "https") return false;
    const std::string prefix = lower_ascii(url.substr(0, std::min<std::size_t>(url.size(), 8)));
    return prefix.rfind("http://", 0) == 0 || prefix.rfind("https://", 0) == 0;
}

OpenExternalUrlResult open_external_url(
    const std::string& url,
    ExternalUrlLauncher launcher) {
    if (!is_safe_external_url(url)) {
        return {false, "only http/https URLs can be opened externally"};
    }

    std::string error;
    bool ok = launcher ? launcher(url, error) : platform_open_external_url(url, error);
    if (!ok) {
        if (error.empty()) error = "failed to open external URL";
        return {false, error};
    }
    return {true, {}};
}

} // namespace acecode::desktop
