#pragma once

// 嵌入的 winpty-agent.exe 字节(定义由 cmake/acecode_bin2cpp.cmake 生成,
// 见 cmake/acecode_winpty.cmake)。仅 Windows 链入(winpty_static)。

#ifdef _WIN32

#include <cstddef>

namespace acecode {

const unsigned char* winpty_agent_data();
std::size_t winpty_agent_size();

} // namespace acecode

#endif // _WIN32
