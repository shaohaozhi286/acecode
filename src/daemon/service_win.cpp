#include "service_win.hpp"

#ifdef _WIN32

#include "worker.hpp"
#include "../config/config.hpp"
#include "../utils/logger.hpp"
#include "../utils/paths.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <atomic>
#include <iostream>
#include <string>
#include <thread>

namespace acecode::daemon::service_win {

namespace {

// SERVICE_STATUS 是 SCM 协议的核心。每次状态切换都要 SetServiceStatus,SCM
// 用它判断服务是否健康 / 控制是否能下达。这两个全局变量只由 SCM 线程访问
// (service_main + service_ctrl_handler 同一线程序列化),不需要互斥。
SERVICE_STATUS        g_status{};
SERVICE_STATUS_HANDLE g_status_handle = nullptr;

const char* status_state_name(DWORD s) {
    switch (s) {
        case SERVICE_STOPPED:          return "STOPPED";
        case SERVICE_START_PENDING:    return "START_PENDING";
        case SERVICE_STOP_PENDING:     return "STOP_PENDING";
        case SERVICE_RUNNING:          return "RUNNING";
        case SERVICE_CONTINUE_PENDING: return "CONTINUE_PENDING";
        case SERVICE_PAUSE_PENDING:    return "PAUSE_PENDING";
        case SERVICE_PAUSED:           return "PAUSED";
        default:                       return "UNKNOWN";
    }
}

void set_status(DWORD state, DWORD exit_code = NO_ERROR, DWORD wait_hint_ms = 0) {
    g_status.dwCurrentState = state;
    g_status.dwWin32ExitCode = exit_code;
    g_status.dwWaitHint = wait_hint_ms;

    if (state == SERVICE_START_PENDING || state == SERVICE_STOP_PENDING) {
        g_status.dwCheckPoint += 1; // SCM 用 checkpoint 判断 PENDING 是否在推进
    } else {
        g_status.dwCheckPoint = 0;
    }

    if (state == SERVICE_RUNNING) {
        g_status.dwControlsAccepted = SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN;
    } else {
        g_status.dwControlsAccepted = 0; // PENDING / STOPPED 状态不接受 control
    }

    BOOL ok = SetServiceStatus(g_status_handle, &g_status);
    // logger 在 service_main 的早期阶段(RegisterServiceCtrlHandlerEx 之后、
    // init_with_rotation 之前)会被调一次 set_status(START_PENDING) — 那时 LOG_*
    // 落空,但不影响后续每一次状态切换都被记录
    LOG_INFO(std::string("[service] SetServiceStatus(") + status_state_name(state) +
             ", exit_code=" + std::to_string(exit_code) +
             ", wait_hint_ms=" + std::to_string(wait_hint_ms) +
             ", checkpoint=" + std::to_string(g_status.dwCheckPoint) +
             ") -> " + (ok ? "OK" : "FAILED err=" + std::to_string(GetLastError())));
}

DWORD WINAPI service_ctrl_handler(DWORD ctrl, DWORD /*evt*/, LPVOID /*evd*/, LPVOID /*ctx*/) {
    LOG_INFO("[service] ctrl_handler received ctrl=" + std::to_string(ctrl));
    switch (ctrl) {
        case SERVICE_CONTROL_STOP:
        case SERVICE_CONTROL_SHUTDOWN:
            // 30s wait_hint — server.stop() + heartbeat.stop() + cleanup 通常 <1s,
            // 给足余量防止 SCM 误判我们卡住
            LOG_INFO("[service] STOP/SHUTDOWN received — set STOP_PENDING + request_worker_termination");
            set_status(SERVICE_STOP_PENDING, NO_ERROR, /*wait_hint=*/30000);
            request_worker_termination();
            return NO_ERROR;
        case SERVICE_CONTROL_INTERROGATE:
            LOG_INFO("[service] INTERROGATE — re-asserting current state");
            SetServiceStatus(g_status_handle, &g_status);
            return NO_ERROR;
        default:
            LOG_WARN("[service] unhandled ctrl code " + std::to_string(ctrl) + " — returning ERROR_CALL_NOT_IMPLEMENTED");
            return ERROR_CALL_NOT_IMPLEMENTED;
    }
}

void WINAPI service_main(DWORD /*argc*/, LPWSTR* /*argv*/) {
    // (1) 注册控制 handler — 必须放在 SCM 协议要求的第一步,先于任何 SetServiceStatus。
    //     失败几乎不可能(分配失败 / 内核句柄耗尽);此时日志 / SCM 通信都没法做,
    //     OutputDebugString 让 DebugView 能抓到这条
    g_status_handle = RegisterServiceCtrlHandlerExW(
        SERVICE_NAME_W, service_ctrl_handler, nullptr);
    if (!g_status_handle) {
        OutputDebugStringW(L"[acecode service] RegisterServiceCtrlHandlerExW failed; cannot proceed\n");
        return;
    }

    g_status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    g_status.dwServiceSpecificExitCode = 0;
    g_status.dwCheckPoint = 0;
    set_status(SERVICE_START_PENDING, NO_ERROR, /*wait_hint=*/10000);

    // (2) 把数据根切到 ProgramData,然后**立刻**初始化 logger 到
    //     %PROGRAMDATA%\acecode\logs\daemon-{YYYY-MM-DD}.log。后续每一步都能记录,
    //     即使 load_config / validate 早早失败也能在文件里看到原因。
    //     base_name 用 "daemon" 与 run_worker 的 init_with_rotation 一致 — 后者
    //     会重新 open 同一个文件(close+reopen 同路径,无数据丢失),所有 service
    //     生命周期日志都落在同一个 daemon-{date}.log 里
    set_run_mode(RunMode::Service);
    Logger::instance().init_with_rotation(get_logs_dir(), "daemon", /*mirror_stderr=*/false);
    Logger::instance().set_level(LogLevel::Dbg);

    LOG_INFO("[service] === service_main entry ===");
    LOG_INFO("[service] data_dir = " + get_acecode_dir());
    LOG_INFO("[service] logs_dir = " + get_logs_dir());
    LOG_INFO("[service] state = START_PENDING (checkpoint 1, wait_hint 10s)");

    AppConfig cfg = load_config();
    LOG_INFO("[service] config loaded: web.bind=" + cfg.web.bind +
             " web.port=" + std::to_string(cfg.web.port) +
             " provider=" + cfg.provider);

    auto errs = validate_config(cfg);
    if (!errs.empty()) {
        for (const auto& e : errs) {
            LOG_ERROR(std::string("[service] config error: ") + e);
        }
        LOG_ERROR("[service] config validation failed — set STOPPED rc=ERROR_INVALID_PARAMETER");
        set_status(SERVICE_STOPPED, ERROR_INVALID_PARAMETER);
        return;
    }
    LOG_INFO("[service] config validation OK");

    WorkerOptions opts;
    opts.foreground = false; // 不镜像 stderr — service 没有 console
    opts.supervised = false; // SCM 自己监督进程崩溃,不走 launcher/supervisor 那套
    opts.dangerous  = false; // service 模式硬关 dangerous(任何场景下都不该开)

    // 进 RUNNING — 严格说 Crow bind 还没成,但我们没有 bind-ready hook;若失败
    // run_worker 会立刻返回非零,后续会被 STOPPED 覆盖。SCM 看到的就是"启动后
    // 立即停止 + exit code",对运维诊断足够。
    set_status(SERVICE_RUNNING);
    LOG_INFO("[service] state = RUNNING — entering run_worker (HTTP server will bind on " +
             cfg.web.bind + ":" + std::to_string(cfg.web.port) + ")");

    int rc = run_worker(opts, cfg);

    LOG_INFO("[service] run_worker returned rc=" + std::to_string(rc));
    DWORD exit_code = (rc == 0) ? NO_ERROR : ERROR_SERVICE_SPECIFIC_ERROR;
    g_status.dwServiceSpecificExitCode = static_cast<DWORD>(rc);
    set_status(SERVICE_STOPPED, exit_code);
    LOG_INFO("[service] === service_main exit (state=STOPPED, exit_code=" +
             std::to_string(exit_code) + " specific_rc=" + std::to_string(rc) + ") ===");
}

// === CLI helpers ===

std::wstring utf8_to_wide(const std::string& s) {
    if (s.empty()) return {};
    int len = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()),
                                   nullptr, 0);
    if (len <= 0) return {};
    std::wstring w(static_cast<size_t>(len), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()),
                        w.data(), len);
    return w;
}

std::string wide_to_utf8(const wchar_t* w, size_t n) {
    if (!w || n == 0) return {};
    int len = WideCharToMultiByte(CP_UTF8, 0, w, static_cast<int>(n),
                                   nullptr, 0, nullptr, nullptr);
    if (len <= 0) return {};
    std::string s(static_cast<size_t>(len), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, static_cast<int>(n), s.data(), len,
                         nullptr, nullptr);
    return s;
}

// 拿当前 exe 全路径(UTF-8)。失败返回空串。
std::string current_exe_path_utf8() {
    wchar_t buf[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return {};
    return wide_to_utf8(buf, n);
}

void print_help(std::ostream& os) {
    os << "Usage: acecode service <subcommand>\n"
       << "\n"
       << "Subcommands:\n"
       << "  install     register AceCodeService as a Windows Service (requires admin)\n"
       << "  uninstall   stop service then deregister (requires admin)\n"
       << "  start       SCM-controlled start\n"
       << "  stop        SCM-controlled stop\n"
       << "  status      print {state, pid}\n";
}

const char* state_name(DWORD s) {
    switch (s) {
        case SERVICE_STOPPED:          return "stopped";
        case SERVICE_START_PENDING:    return "start_pending";
        case SERVICE_STOP_PENDING:     return "stop_pending";
        case SERVICE_RUNNING:          return "running";
        case SERVICE_CONTINUE_PENDING: return "continue_pending";
        case SERVICE_PAUSE_PENDING:    return "pause_pending";
        case SERVICE_PAUSED:           return "paused";
        default:                       return "unknown";
    }
}

int do_install(const std::string& exe_path) {
    std::string exe = exe_path;
    if (exe.empty()) exe = current_exe_path_utf8();
    if (exe.empty()) {
        std::cerr << "service install: cannot resolve current exe path\n";
        return 23;
    }

    // SCM 把 lpBinaryPathName 当 cmdline 直接执行。加引号防 exe 路径含空格。
    std::string cmdline = "\"" + exe + "\" --service-main";
    std::wstring wcmd = utf8_to_wide(cmdline);

    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CREATE_SERVICE);
    if (!scm) {
        DWORD err = GetLastError();
        if (err == ERROR_ACCESS_DENIED) {
            std::cerr << "service install: access denied — "
                      << "please run from an elevated (Administrator) PowerShell\n";
            return 24;
        }
        std::cerr << "service install: OpenSCManager failed (err=" << err << ")\n";
        return 25;
    }

    SC_HANDLE svc = CreateServiceW(
        scm,
        SERVICE_NAME_W,
        DISPLAY_NAME_W,
        SERVICE_ALL_ACCESS,
        SERVICE_WIN32_OWN_PROCESS,
        SERVICE_AUTO_START,         // 开机自动启动 — 比登录框还早
        SERVICE_ERROR_NORMAL,
        wcmd.c_str(),
        nullptr,                    // 没有 load order group
        nullptr,                    // 不要 tag id
        nullptr,                    // 没有依赖
        nullptr,                    // lpServiceStartName=NULL → LocalSystem 身份
        nullptr                     // 没有密码
    );

    if (!svc) {
        DWORD err = GetLastError();
        CloseServiceHandle(scm);
        if (err == ERROR_SERVICE_EXISTS || err == ERROR_DUPLICATE_SERVICE_NAME) {
            std::cerr << "service install: AceCodeService already registered; "
                      << "run `acecode service uninstall` first\n";
            return 26;
        }
        if (err == ERROR_ACCESS_DENIED) {
            std::cerr << "service install: access denied — "
                      << "please run from an elevated (Administrator) PowerShell\n";
            return 24;
        }
        std::cerr << "service install: CreateService failed (err=" << err << ")\n";
        return 27;
    }

    CloseServiceHandle(svc);
    CloseServiceHandle(scm);
    std::cout << "AceCodeService installed (auto-start, LocalSystem identity)\n"
              << "Data dir: %PROGRAMDATA%\\acecode\\\n"
              << "Run `acecode service start` to start it now (or it will auto-start on next reboot).\n";
    return 0;
}

int do_uninstall() {
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!scm) {
        DWORD err = GetLastError();
        if (err == ERROR_ACCESS_DENIED) {
            std::cerr << "service uninstall: access denied — "
                      << "please run from an elevated (Administrator) PowerShell\n";
            return 24;
        }
        std::cerr << "service uninstall: OpenSCManager failed (err=" << err << ")\n";
        return 25;
    }

    SC_HANDLE svc = OpenServiceW(scm, SERVICE_NAME_W,
                                  SERVICE_STOP | DELETE | SERVICE_QUERY_STATUS);
    if (!svc) {
        DWORD err = GetLastError();
        CloseServiceHandle(scm);
        if (err == ERROR_SERVICE_DOES_NOT_EXIST) {
            std::cerr << "AceCodeService not installed\n";
            return 28;
        }
        std::cerr << "service uninstall: OpenService failed (err=" << err << ")\n";
        return 29;
    }

    // 先优雅 stop;不在乎是否成功(可能已经 stopped)
    SERVICE_STATUS st{};
    if (ControlService(svc, SERVICE_CONTROL_STOP, &st)) {
        for (int i = 0; i < 60; ++i) { // 最多 30s 等 stopped
            Sleep(500);
            if (!QueryServiceStatus(svc, &st)) break;
            if (st.dwCurrentState == SERVICE_STOPPED) break;
        }
    }

    if (!DeleteService(svc)) {
        DWORD err = GetLastError();
        CloseServiceHandle(svc);
        CloseServiceHandle(scm);
        std::cerr << "service uninstall: DeleteService failed (err=" << err << ")\n";
        return 30;
    }

    CloseServiceHandle(svc);
    CloseServiceHandle(scm);
    std::cout << "AceCodeService uninstalled\n"
              << "Note: data directory %PROGRAMDATA%\\acecode\\ left in place; "
              << "delete manually if no longer needed.\n";
    return 0;
}

int do_start() {
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!scm) {
        std::cerr << "service start: OpenSCManager failed (err=" << GetLastError() << ")\n";
        return 25;
    }
    SC_HANDLE svc = OpenServiceW(scm, SERVICE_NAME_W,
                                  SERVICE_START | SERVICE_QUERY_STATUS);
    if (!svc) {
        DWORD err = GetLastError();
        CloseServiceHandle(scm);
        if (err == ERROR_SERVICE_DOES_NOT_EXIST) {
            std::cerr << "AceCodeService not installed; "
                      << "run `acecode service install` first\n";
            return 28;
        }
        std::cerr << "service start: OpenService failed (err=" << err << ")\n";
        return 29;
    }
    if (!StartServiceW(svc, 0, nullptr)) {
        DWORD err = GetLastError();
        CloseServiceHandle(svc);
        CloseServiceHandle(scm);
        if (err == ERROR_SERVICE_ALREADY_RUNNING) {
            std::cout << "AceCodeService already running\n";
            return 0;
        }
        if (err == ERROR_ACCESS_DENIED) {
            std::cerr << "service start: access denied — "
                      << "please run from an elevated (Administrator) PowerShell\n";
            return 24;
        }
        std::cerr << "service start: StartService failed (err=" << err << ")\n";
        return 31;
    }
    CloseServiceHandle(svc);
    CloseServiceHandle(scm);
    std::cout << "AceCodeService start requested (use `acecode service status` to verify)\n";
    return 0;
}

int do_stop() {
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!scm) {
        std::cerr << "service stop: OpenSCManager failed (err=" << GetLastError() << ")\n";
        return 25;
    }
    SC_HANDLE svc = OpenServiceW(scm, SERVICE_NAME_W,
                                  SERVICE_STOP | SERVICE_QUERY_STATUS);
    if (!svc) {
        DWORD err = GetLastError();
        CloseServiceHandle(scm);
        if (err == ERROR_SERVICE_DOES_NOT_EXIST) {
            std::cerr << "AceCodeService not installed\n";
            return 28;
        }
        std::cerr << "service stop: OpenService failed (err=" << err << ")\n";
        return 29;
    }
    SERVICE_STATUS st{};
    if (!ControlService(svc, SERVICE_CONTROL_STOP, &st)) {
        DWORD err = GetLastError();
        CloseServiceHandle(svc);
        CloseServiceHandle(scm);
        if (err == ERROR_SERVICE_NOT_ACTIVE) {
            std::cout << "AceCodeService already stopped\n";
            return 0;
        }
        if (err == ERROR_ACCESS_DENIED) {
            std::cerr << "service stop: access denied — "
                      << "please run from an elevated (Administrator) PowerShell\n";
            return 24;
        }
        std::cerr << "service stop: ControlService failed (err=" << err << ")\n";
        return 32;
    }
    CloseServiceHandle(svc);
    CloseServiceHandle(scm);
    std::cout << "AceCodeService stop signal sent\n";
    return 0;
}

int do_status() {
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!scm) {
        std::cerr << "service status: OpenSCManager failed (err=" << GetLastError() << ")\n";
        return 25;
    }
    SC_HANDLE svc = OpenServiceW(scm, SERVICE_NAME_W, SERVICE_QUERY_STATUS);
    if (!svc) {
        DWORD err = GetLastError();
        CloseServiceHandle(scm);
        if (err == ERROR_SERVICE_DOES_NOT_EXIST) {
            std::cout << "AceCodeService not installed\n";
            return 1;
        }
        std::cerr << "service status: OpenService failed (err=" << err << ")\n";
        return 29;
    }
    SERVICE_STATUS_PROCESS sp{};
    DWORD bytes_needed = 0;
    if (!QueryServiceStatusEx(svc, SC_STATUS_PROCESS_INFO,
                               reinterpret_cast<LPBYTE>(&sp), sizeof(sp),
                               &bytes_needed)) {
        DWORD err = GetLastError();
        CloseServiceHandle(svc);
        CloseServiceHandle(scm);
        std::cerr << "service status: QueryServiceStatusEx failed (err=" << err << ")\n";
        return 33;
    }
    CloseServiceHandle(svc);
    CloseServiceHandle(scm);

    std::cout << "AceCodeService\n"
              << "  state: " << state_name(sp.dwCurrentState) << "\n";
    if (sp.dwProcessId != 0) {
        std::cout << "  pid:   " << sp.dwProcessId << "\n";
    }
    return (sp.dwCurrentState == SERVICE_RUNNING) ? 0 : 2;
}

} // namespace

int run_service_main_dispatcher() {
    // 此时 logger 还没初始化(service_main 内部才会 init);用 OutputDebugString
    // 让 DebugView (sysinternals) 能抓到 dispatcher 失败前的诊断信息
    OutputDebugStringW(L"[acecode service] run_service_main_dispatcher: calling StartServiceCtrlDispatcherW\n");

    SERVICE_TABLE_ENTRYW table[] = {
        { const_cast<LPWSTR>(SERVICE_NAME_W), service_main },
        { nullptr, nullptr }
    };
    if (!StartServiceCtrlDispatcherW(table)) {
        DWORD err = GetLastError();
        if (err == ERROR_FAILED_SERVICE_CONTROLLER_CONNECT) {
            OutputDebugStringW(L"[acecode service] StartServiceCtrlDispatcherW failed: ERROR_FAILED_SERVICE_CONTROLLER_CONNECT (called outside SCM)\n");
            std::cerr << "--service-main is for SCM only; do not invoke directly\n";
            return 21;
        }
        std::wstring msg = L"[acecode service] StartServiceCtrlDispatcherW failed err=" + std::to_wstring(err) + L"\n";
        OutputDebugStringW(msg.c_str());
        std::cerr << "StartServiceCtrlDispatcher failed (err=" << err << ")\n";
        return 22;
    }
    OutputDebugStringW(L"[acecode service] run_service_main_dispatcher: returning 0\n");
    return 0;
}

int run_cli(const std::vector<std::string>& tokens, const std::string& exe_path) {
    if (tokens.empty() || tokens[0] == "--help" || tokens[0] == "-h" ||
        tokens[0] == "help") {
        print_help(std::cout);
        return tokens.empty() ? 11 : 0;
    }
    const std::string& sub = tokens[0];
    if (sub == "install")   return do_install(exe_path);
    if (sub == "uninstall") return do_uninstall();
    if (sub == "start")     return do_start();
    if (sub == "stop")      return do_stop();
    if (sub == "status")    return do_status();
    std::cerr << "unknown service subcommand: " << sub << "\n\n";
    print_help(std::cerr);
    return 10;
}

} // namespace acecode::daemon::service_win

#endif // _WIN32
