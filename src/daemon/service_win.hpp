#pragma once

// Windows Service 包装(spec design.md Decision 8)。Windows-only — 整个 TU
// 用 #ifdef _WIN32 包起来,Linux/macOS 编译时为空。
//
// 三个外部入口:
//   1. ServiceMain dispatcher: SCM 启动我们的 exe 时,argv 含 --service-main。
//      main.cpp 检测到后调 run_service_main_dispatcher(),阻塞直到服务退出。
//      内部走 StartServiceCtrlDispatcherW + service_main + RegisterServiceCtrlHandlerExW
//      标准 SCM 协议;首行调 set_run_mode(RunMode::Service) 把数据根切到 ProgramData。
//
//   2. CLI 子命令: `acecode service install/uninstall/start/stop/status`。
//      install 走 SCM CreateService(LocalSystem 身份,无密码,固定服务名 AceCodeService);
//      其它子命令走 ControlService / QueryServiceStatusEx。
//
//   3. 常量:服务名 / 显示名硬编码,不让 install 自定义(避免一台机器装多份产生冲突)。
//
// 安全与权限: install/uninstall 需要管理员权限(ERROR_ACCESS_DENIED 时给清晰提示);
// start/stop/status 普通用户也能调(只读 SCM)。

#ifdef _WIN32

#include <string>
#include <vector>

namespace acecode::daemon::service_win {

// 服务标识 — 硬编码,不暴露给 install --name 自定义。
inline constexpr const wchar_t* SERVICE_NAME_W = L"AceCodeService";
inline constexpr const wchar_t* DISPLAY_NAME_W = L"ACECode Background Agent";

// SCM 启动我们时(argv 含 --service-main),main.cpp 调这个;阻塞直到服务退出。
// 返回进程退出码;非 SCM 上下文调用会返回非零并打印错误(StartServiceCtrlDispatcher
// 返回 ERROR_FAILED_SERVICE_CONTROLLER_CONNECT)。
int run_service_main_dispatcher();

// `acecode service <sub>` CLI 派发。tokens 不含 argv[0] 与 "service" 子命令本身。
// exe_path 用于 install 时填充 lpBinaryPathName。
int run_cli(const std::vector<std::string>& tokens, const std::string& exe_path);

} // namespace acecode::daemon::service_win

#endif // _WIN32
