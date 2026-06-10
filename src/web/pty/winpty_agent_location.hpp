#pragma once

// winpty agent 路径的进程级覆盖。详见 winpty_agent_location.cpp 头注释。
// 仅 Windows 有实现;非 Windows 平台不应引用本头。

#ifdef _WIN32

#include <string>

namespace acecode {

// 设置 winpty-agent.exe 的绝对路径(由 WinptyBackend 在首次创建会话、
// 把嵌入的 agent 释放到 <data_dir>/bin/ 之后调用)。线程安全。
void set_winpty_agent_path(const std::wstring& absolute_agent_exe_path);

// 读取当前覆盖值(单测用)。未设置时为空。
std::wstring winpty_agent_path_for_testing();

} // namespace acecode

#endif // _WIN32
