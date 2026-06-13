#pragma once

#include <string>

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
#  include <sys/types.h>
#endif

namespace acecode {

// Opens a file with the system's default application.
// On Linux: xdg-open, macOS: open, Windows: ShellExecuteW.
// Fire-and-forget — does not wait for the application to close.
// Returns true if the launch syscall succeeded.
inline bool platform_open_file(const std::string& path) {
    if (path.empty()) return false;
#ifdef _WIN32
    const int wide_len = MultiByteToWideChar(CP_UTF8, 0, path.c_str(),
                                             static_cast<int>(path.size()),
                                             nullptr, 0);
    if (wide_len <= 0) return false;
    std::wstring wide_path(static_cast<size_t>(wide_len), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, path.c_str(), static_cast<int>(path.size()),
                        &wide_path[0], wide_len);
    const auto code = reinterpret_cast<intptr_t>(
        ::ShellExecuteW(nullptr, L"open", wide_path.c_str(),
                        nullptr, nullptr, SW_SHOWNORMAL));
    return code > 32;
#else
#  ifdef __APPLE__
    const char* opener = "open";
#  else
    const char* opener = "xdg-open";
#  endif
    pid_t pid = ::fork();
    if (pid < 0) return false;
    if (pid == 0) {
        ::execlp(opener, opener, path.c_str(), static_cast<char*>(nullptr));
        ::_exit(127);
    }
    return true;
#endif
}

} // namespace acecode
