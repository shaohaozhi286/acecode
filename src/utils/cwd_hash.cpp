#include "cwd_hash.hpp"

#include "encoding.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <iomanip>
#include <optional>
#include <sstream>
#include <string_view>

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

namespace {

// FNV-1a 64-bit。无外部依赖,与 src/session/session_storage.cpp 历史实现 byte-byte 等价。
std::uint64_t fnv1a_64(const std::string& data) {
    std::uint64_t hash = 14695981039346656037ULL;
    for (unsigned char c : data) {
        hash ^= c;
        hash *= 1099511628211ULL;
    }
    return hash;
}

#ifdef _WIN32
std::string wide_to_utf8(const std::wstring& w) {
    if (w.empty()) return {};
    int len = ::WideCharToMultiByte(CP_UTF8, 0, w.data(),
                                    static_cast<int>(w.size()),
                                    nullptr, 0, nullptr, nullptr);
    if (len <= 0) {
        std::string fallback;
        fallback.reserve(w.size());
        for (wchar_t ch : w) {
            fallback.push_back(ch >= 0 && ch <= 0x7f ? static_cast<char>(ch) : '?');
        }
        return fallback;
    }
    std::string out(static_cast<std::size_t>(len), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()),
                          out.data(), len, nullptr, nullptr);
    return out;
}

std::optional<std::string> final_path_identity(const std::string& path) {
    std::wstring wpath = acecode::utf8_to_wide(path);
    if (wpath.empty()) return std::nullopt;
    HANDLE h = ::CreateFileW(
        wpath.c_str(),
        0,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS,
        nullptr);
    if (h == INVALID_HANDLE_VALUE) return std::nullopt;

    DWORD needed = ::GetFinalPathNameByHandleW(
        h, nullptr, 0, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    if (needed == 0) {
        ::CloseHandle(h);
        return std::nullopt;
    }

    std::wstring buffer(static_cast<std::size_t>(needed) + 1, L'\0');
    DWORD written = ::GetFinalPathNameByHandleW(
        h, buffer.data(), static_cast<DWORD>(buffer.size()),
        FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    ::CloseHandle(h);
    if (written == 0) return std::nullopt;
    buffer.resize(written);

    constexpr std::wstring_view kDosPrefix = L"\\\\?\\";
    constexpr std::wstring_view kUncPrefix = L"\\\\?\\UNC\\";
    if (buffer.rfind(kUncPrefix, 0) == 0) {
        buffer = L"\\\\" + buffer.substr(kUncPrefix.size());
    } else if (buffer.rfind(kDosPrefix, 0) == 0) {
        buffer = buffer.substr(kDosPrefix.size());
    }
    return wide_to_utf8(buffer);
}
#endif

} // namespace

std::string normalize_cwd_for_hash(const std::string& cwd) {
    namespace fs = std::filesystem;

    std::string normalized = cwd;
    if (!normalized.empty()) {
        std::error_code ec;
#ifdef _WIN32
        if (auto final_path = final_path_identity(normalized)) {
            normalized = *final_path;
        } else {
#endif
        fs::path p(normalized);
        // Only canonicalize existing paths. Non-existing synthetic test paths
        // keep their historical string identity.
        if (fs::exists(p, ec)) {
            auto canonical = fs::weakly_canonical(p, ec);
            if (!ec && !canonical.empty()) {
                normalized = canonical.string();
            }
        }
#ifdef _WIN32
        }
#endif
    }

    // 1. 反斜杠 → 正斜杠(跨平台一致)
    for (auto& c : normalized) {
        if (c == '\\') c = '/';
    }
    // 2. 全部小写(Windows / macOS 大小写不敏感的兜底;Linux 严格大小写但路径用户自己掌控)
    std::transform(normalized.begin(), normalized.end(), normalized.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    // 3. 去尾斜杠("foo/" 与 "foo" 算同一目录)
    while (normalized.size() > 1 && normalized.back() == '/') {
        normalized.pop_back();
    }
    return normalized;
}

std::string compute_cwd_hash(const std::string& cwd) {
    std::string normalized = normalize_cwd_for_hash(cwd);

    std::uint64_t h = fnv1a_64(normalized);
    std::ostringstream oss;
    oss << std::hex << std::setfill('0') << std::setw(16) << h;
    return oss.str();
}

} // namespace acecode
