// findAgentProgram() 的替换实现 — 取代上游 libwinpty/AgentLocation.cc
// (cmake/acecode_winpty.cmake 刻意不编译上游那份,见该文件头注释)。
//
// 上游行为:在"包含当前模块的目录"找 winpty-agent.exe。静态链接进
// acecode.exe 后即安装目录 — 通常不可写,无法放置释放出来的 agent。
// 这里加一层进程级覆盖:PtyBackend 在创建 winpty 会话前把释放到
// <data_dir>/bin/ 的 agent 路径 set 进来;未设置时回退上游同目录行为
// (开发态/单测把 agent 拷到 exe 旁即可直接工作)。
#ifdef _WIN32

#include "winpty_agent_location.hpp"

#include <windows.h>

#include <mutex>
#include <string>

#include "AgentLocation.h"
#include "LibWinptyException.h"

namespace {

std::mutex g_agent_path_mu;
std::wstring g_agent_path_override;

HMODULE current_module() {
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(&current_module), &module)) {
        return nullptr;
    }
    return module;
}

std::wstring module_directory() {
    wchar_t path[4096] = {};
    DWORD size = GetModuleFileNameW(current_module(), path, 4096);
    if (size == 0 || size >= 4096) return L"";
    std::wstring full(path, size);
    auto pos = full.find_last_of(L"\\/");
    if (pos == std::wstring::npos) return L"";
    return full.substr(0, pos);
}

bool path_exists(const std::wstring& path) {
    return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

} // namespace

namespace acecode {

void set_winpty_agent_path(const std::wstring& absolute_agent_exe_path) {
    std::lock_guard<std::mutex> lock(g_agent_path_mu);
    g_agent_path_override = absolute_agent_exe_path;
}

std::wstring winpty_agent_path_for_testing() {
    std::lock_guard<std::mutex> lock(g_agent_path_mu);
    return g_agent_path_override;
}

} // namespace acecode

// 上游签名(libwinpty/AgentLocation.h),winpty.cc 的 winpty_open 路径调用。
std::wstring findAgentProgram() {
    {
        std::lock_guard<std::mutex> lock(g_agent_path_mu);
        if (!g_agent_path_override.empty()) {
            if (path_exists(g_agent_path_override)) {
                return g_agent_path_override;
            }
            throw LibWinptyException(
                WINPTY_ERROR_AGENT_EXE_MISSING,
                (L"configured agent executable does not exist: '" +
                 g_agent_path_override + L"'")
                    .c_str());
        }
    }
    std::wstring fallback = module_directory() + L"\\winpty-agent.exe";
    if (!path_exists(fallback)) {
        throw LibWinptyException(
            WINPTY_ERROR_AGENT_EXE_MISSING,
            (L"agent executable does not exist: '" + fallback + L"'").c_str());
    }
    return fallback;
}

#endif // _WIN32
