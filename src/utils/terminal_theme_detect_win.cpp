#ifdef _WIN32

#include "terminal_theme_detect.hpp"
#include "logger.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstdlib>
#include <string>

namespace acecode {

// ---- OSC 11 探测(Windows) ----
// 只在 Windows Terminal(WT_SESSION 存在)下尝试 — conhost 不支持 OSC 11。
// 发 \033]11;?\033\\ 到 stdout,200ms 超时读 stdin,从 KEY_EVENT 中拼响应串。

std::optional<std::string> probe_osc11_platform() {
    // conhost 不支持 OSC 11,只在 Windows Terminal 下尝试
    const char* wt = std::getenv("WT_SESSION");
    if (!wt || wt[0] == '\0') {
        return std::nullopt;
    }

    HANDLE hOutput = GetStdHandle(STD_OUTPUT_HANDLE);
    HANDLE hInput  = GetStdHandle(STD_INPUT_HANDLE);
    if (!hOutput || hOutput == INVALID_HANDLE_VALUE ||
        !hInput  || hInput  == INVALID_HANDLE_VALUE) {
        return std::nullopt;
    }

    // 保存并设置 console mode
    DWORD old_out_mode = 0, old_in_mode = 0;
    if (!GetConsoleMode(hOutput, &old_out_mode) ||
        !GetConsoleMode(hInput,  &old_in_mode)) {
        return std::nullopt;
    }

    SetConsoleMode(hOutput,
                   old_out_mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
    SetConsoleMode(hInput,
                   (old_in_mode | ENABLE_VIRTUAL_TERMINAL_INPUT)
                   & ~ENABLE_LINE_INPUT
                   & ~ENABLE_ECHO_INPUT
                   & ~ENABLE_PROCESSED_INPUT);

    // 清空输入缓冲
    FlushConsoleInputBuffer(hInput);

    // 发送 OSC 11 查询
    const char query[] = "\033]11;?\033\\";
    DWORD written = 0;
    WriteConsoleA(hOutput, query, sizeof(query) - 1, &written, nullptr);

    // 读响应(200ms 总超时)
    std::string response;
    DWORD deadline = GetTickCount() + 200;

    while (true) {
        DWORD now = GetTickCount();
        DWORD remaining = (now < deadline) ? (deadline - now) : 0;
        if (remaining == 0) break;

        DWORD ret = WaitForSingleObject(hInput, remaining);
        if (ret != WAIT_OBJECT_0) break;

        INPUT_RECORD records[32];
        DWORD count = 0;
        if (!ReadConsoleInputA(hInput, records, 32, &count)) break;

        for (DWORD i = 0; i < count; ++i) {
            if (records[i].EventType == KEY_EVENT &&
                records[i].Event.KeyEvent.bKeyDown) {
                char c = records[i].Event.KeyEvent.uChar.AsciiChar;
                if (c) response += c;
            }
        }

        // 检查是否收到完整响应(\033\\ 或 \007 结尾)
        if (response.find("\033\\") != std::string::npos ||
            response.find('\007')  != std::string::npos) {
            break;
        }
    }

    // 恢复 console mode
    SetConsoleMode(hOutput, old_out_mode);
    SetConsoleMode(hInput,  old_in_mode);

    if (response.empty()) {
        return std::nullopt;
    }

    // 从响应中提取 "rgb:..." 部分
    // 响应格式: \033]11;rgb:RRRR/GGGG/BBBB\033\\ 或 ...BEL
    auto rgb_pos = response.find("rgb:");
    if (rgb_pos == std::string::npos) {
        LOG_WARN("[theme_detect] OSC 11 response missing 'rgb:': " + response);
        return std::nullopt;
    }

    // 截到终止符
    auto end = response.find('\033', rgb_pos);
    if (end == std::string::npos) {
        end = response.find('\007', rgb_pos);
    }
    if (end == std::string::npos) {
        end = response.size();
    }

    return response.substr(rgb_pos, end - rgb_pos);
}

// ---- 系统暗色模式(Windows 注册表) ----
// HKCU\SOFTWARE\Microsoft\Windows\CurrentVersion\Themes\Personalize
//   AppsUseLightTheme: DWORD, 0=暗色 1=亮色

std::optional<bool> probe_system_dark_mode() {
    HKEY key = nullptr;
    LSTATUS status = RegOpenKeyExW(
        HKEY_CURRENT_USER,
        L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
        0, KEY_READ, &key);
    if (status != ERROR_SUCCESS) {
        return std::nullopt;
    }

    DWORD value = 0;
    DWORD size = sizeof(value);
    DWORD type = 0;
    status = RegQueryValueExW(key, L"AppsUseLightTheme", nullptr,
                              &type, reinterpret_cast<LPBYTE>(&value), &size);
    RegCloseKey(key);

    if (status != ERROR_SUCCESS || type != REG_DWORD) {
        return std::nullopt;
    }

    // 0 = dark mode, 1 = light mode → 返回 is_dark
    return value == 0;
}

} // namespace acecode

#endif // _WIN32
