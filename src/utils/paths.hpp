#pragma once

// 进程级 RunMode 抽象 + 跨平台数据目录解析(spec design.md Decision 8)。
//
// 目的: ServiceMain 入口需要在做任何文件 IO 之前把数据根从 ~/.acecode/ 切到
// 系统级位置(Windows ProgramData / macOS /Library/Application Support /
// Linux /var/lib),否则 LocalSystem 身份的 daemon 写的产物落在 systemprofile
// 下,TUI 看不见。User 模式(默认)行为与历史完全一致 — TUI 与 standalone
// daemon 不受影响。
//
// set_run_mode() 是 once-only — 二次调用打 LOG_WARN 后忽略;这是一道防呆,避免
// 进程半路改根目录把状态散到两套路径下。测试通过 override_run_mode_for_test()
// 绕过 once 保护,fixture TearDown 应调 reset_run_mode_for_test() 清场。

#include <string>

namespace acecode {

enum class RunMode {
    User    = 0, // 默认 — TUI / standalone daemon / `acecode daemon --foreground`
    Service = 1, // SCM ServiceMain 拉起的 worker
};

// 进程启动早期调一次。第二次起被吞掉(LOG_WARN 提示)。
void set_run_mode(RunMode mode);

// 当前进程的 RunMode(默认 User)。
RunMode get_run_mode();

// 纯函数: 算给定 mode 下的数据根目录(不创建目录,调用方自己 create_directories)。
//
// Windows | User    : %USERPROFILE%\.acecode\ (USERPROFILE 缺失退到 HOMEDRIVE+HOMEPATH,再缺失退到 .\.acecode)
//         | Service : %PROGRAMDATA%\acecode\  (≈ C:\ProgramData\acecode\,缺失退到字面 C:\ProgramData)
// macOS   | User    : $HOME/.acecode/         (缺失退到 ./.acecode)
//         | Service : /Library/Application Support/acecode
// Linux   | User    : $HOME/.acecode/         (缺失退到 ./.acecode)
//         | Service : /var/lib/acecode
std::string resolve_data_dir(RunMode mode);

// === 测试专用 helper(只在测试代码用,生产路径不应调) ===

// 直接覆盖当前 RunMode,绕过 once-only 保护;返回原值方便 test fixture 还原。
RunMode override_run_mode_for_test(RunMode mode);

// 把 RunMode 与 once-only 标记都重置回初始(User + 未 set 过)。
// fixture TearDown 调,确保测试间互不污染。
void reset_run_mode_for_test();

} // namespace acecode
