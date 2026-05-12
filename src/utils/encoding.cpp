#include "encoding.hpp"

#include <cstdlib>

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#endif

namespace acecode {

#ifdef _WIN32
std::string codepage_to_utf8(const std::string& src, unsigned int codepage) {
    if (src.empty()) return src;

    // First convert to wide string (UTF-16)
    int wide_len = MultiByteToWideChar(codepage, 0, src.c_str(), static_cast<int>(src.size()), nullptr, 0);
    if (wide_len <= 0) return src;

    std::wstring wide(static_cast<size_t>(wide_len), L'\0');
    MultiByteToWideChar(codepage, 0, src.c_str(), static_cast<int>(src.size()), wide.data(), wide_len);

    // Then convert wide string to UTF-8
    int utf8_len = WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), wide_len, nullptr, 0, nullptr, nullptr);
    if (utf8_len <= 0) return src;

    std::string utf8(static_cast<size_t>(utf8_len), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), wide_len, utf8.data(), utf8_len, nullptr, nullptr);
    return utf8;
}

namespace {

std::wstring multibyte_to_wide(const std::string& src,
                               unsigned int codepage,
                               unsigned long flags = 0) {
    if (src.empty()) return {};
    int wide_len = MultiByteToWideChar(codepage, flags, src.data(),
                                       static_cast<int>(src.size()),
                                       nullptr, 0);
    if (wide_len <= 0) return {};

    std::wstring wide(static_cast<size_t>(wide_len), L'\0');
    MultiByteToWideChar(codepage, flags, src.data(), static_cast<int>(src.size()),
                        wide.data(), wide_len);
    return wide;
}

} // namespace

std::wstring utf8_to_wide(const std::string& src) {
    if (src.empty()) return {};

    // Most ACECode persisted paths are UTF-8. Some older Windows paths came
    // from narrow filesystem APIs and are in the active codepage, so keep an
    // ACP fallback to avoid stranding existing metadata.
    std::wstring wide = multibyte_to_wide(src, CP_UTF8, MB_ERR_INVALID_CHARS);
    if (!wide.empty()) return wide;
    return multibyte_to_wide(src, CP_ACP);
}

std::string wide_to_utf8(const std::wstring& wide) {
    if (wide.empty()) return {};
    int utf8_len = WideCharToMultiByte(CP_UTF8, 0, wide.data(),
                                       static_cast<int>(wide.size()),
                                       nullptr, 0, nullptr, nullptr);
    if (utf8_len <= 0) return {};

    std::string utf8(static_cast<size_t>(utf8_len), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()),
                        utf8.data(), utf8_len, nullptr, nullptr);
    return utf8;
}
#endif

bool getenv_utf8(const char* name, std::string& out) {
    out.clear();
    if (!name || !*name) return false;
#ifdef _WIN32
    std::wstring wname = utf8_to_wide(name);
    if (wname.empty()) return false;

    SetLastError(ERROR_SUCCESS);
    DWORD needed = GetEnvironmentVariableW(wname.c_str(), nullptr, 0);
    if (needed == 0) {
        return GetLastError() != ERROR_ENVVAR_NOT_FOUND;
    }

    std::wstring value(static_cast<size_t>(needed), L'\0');
    DWORD written = GetEnvironmentVariableW(wname.c_str(), value.data(), needed);
    if (written == 0 && GetLastError() != ERROR_SUCCESS) return false;
    value.resize(static_cast<size_t>(written));
    out = wide_to_utf8(value);
    return true;
#else
    const char* value = std::getenv(name);
    if (!value) return false;
    out = value;
    return true;
#endif
}

std::string getenv_utf8(const char* name) {
    std::string out;
    return getenv_utf8(name, out) ? out : std::string{};
}

} // namespace acecode
