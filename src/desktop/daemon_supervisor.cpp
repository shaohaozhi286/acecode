#include "daemon_supervisor.hpp"

#include "../utils/encoding.hpp"
#include "../utils/token.hpp"

#include <chrono>
#include <thread>
#include <sstream>
#include <vector>

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  pragma comment(lib, "ws2_32.lib")
#endif

namespace acecode::desktop {

namespace {

#ifdef _WIN32
// 单次 TCP connect 探测。成功 = daemon listen 中。
bool tcp_probe(int port) {
    SOCKET s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return false;

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<u_short>(port));
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    // 设短超时,避免一次 connect 卡 syn 重试好几秒。这里用 non-blocking +
    // select 也行,但对 loopback 来说 blocking connect 通常 < 5ms,简化处理。
    DWORD tv = 500; // ms
    ::setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&tv), sizeof(tv));
    ::setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&tv), sizeof(tv));

    int rc = ::connect(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    ::closesocket(s);
    return rc == 0;
}

// WSAStartup 守护: 确保整个进程生命周期里 winsock 可用。MVP 阶段直接做一次,
// 不释放 — 进程退出时 OS 会回收。
struct WsaInit {
    WsaInit() {
        WSADATA d{};
        ::WSAStartup(MAKEWORD(2, 2), &d);
    }
};
WsaInit g_wsa_init;

// Quote one argv element per the Windows command-line parsing rules. Even
// though CreateProcessW has a Unicode API, it still takes argv as one mutable
// command-line buffer.
std::wstring quote_arg_w(const std::wstring& s) {
    bool need_quotes = s.empty() || s.find_first_of(L" \t\"") != std::wstring::npos;
    if (!need_quotes) return s;

    std::wstring out;
    out.reserve(s.size() + 2);
    out.push_back(L'"');
    int backslashes = 0;
    for (wchar_t c : s) {
        if (c == L'\\') {
            ++backslashes;
        } else if (c == L'"') {
            for (int i = 0; i < backslashes * 2 + 1; ++i) out.push_back(L'\\');
            out.push_back(L'"');
            backslashes = 0;
        } else {
            for (int i = 0; i < backslashes; ++i) out.push_back(L'\\');
            backslashes = 0;
            out.push_back(c);
        }
    }
    for (int i = 0; i < backslashes * 2; ++i) out.push_back(L'\\');
    out.push_back(L'"');
    return out;
}

std::wstring build_command_line_w(const std::vector<std::wstring>& argv) {
    std::wostringstream oss;
    for (size_t i = 0; i < argv.size(); ++i) {
        if (i) oss << L' ';
        oss << quote_arg_w(argv[i]);
    }
    return oss.str();
}

std::string windows_error_suffix(DWORD err) {
    std::ostringstream oss;
    oss << " (gle=" << err << ")";
    return oss.str();
}
#endif

} // namespace

#ifdef _WIN32

struct DaemonSupervisor::Impl {
    HANDLE job = nullptr;
    HANDLE process = nullptr;
    HANDLE thread = nullptr;
    DWORD  pid = 0;
};

DaemonSupervisor::DaemonSupervisor() : impl_(new Impl()) {}
DaemonSupervisor::~DaemonSupervisor() {
    stop();
    delete impl_;
}

SpawnResult DaemonSupervisor::spawn(const SpawnRequest& req) {
    SpawnResult r;
    if (req.daemon_exe_path.empty()) {
        r.error = "daemon_exe_path empty";
        return r;
    }
    if (req.port <= 0 || req.port > 65535) {
        r.error = "port out of range";
        return r;
    }
    if (req.token.empty()) {
        r.error = "token empty";
        return r;
    }

    std::wstring exe_w = acecode::utf8_to_wide(req.daemon_exe_path);
    if (exe_w.empty()) {
        r.error = "daemon_exe_path cannot be converted to UTF-16";
        return r;
    }

    std::wstring cwd_w;
    LPCWSTR cwd_arg = nullptr;
    if (!req.cwd.empty()) {
        cwd_w = acecode::utf8_to_wide(req.cwd);
        if (cwd_w.empty()) {
            r.error = "cwd cannot be converted to UTF-16: " + req.cwd;
            return r;
        }
        DWORD attrs = ::GetFileAttributesW(cwd_w.c_str());
        if (attrs == INVALID_FILE_ATTRIBUTES) {
            DWORD err = ::GetLastError();
            r.error = "working directory is not accessible: " + req.cwd
                + windows_error_suffix(err);
            return r;
        }
        if ((attrs & FILE_ATTRIBUTE_DIRECTORY) == 0) {
            r.error = "working directory is not a directory: " + req.cwd;
            return r;
        }
        cwd_arg = cwd_w.c_str();
    }

    // 1) Job Object: KILL_ON_JOB_CLOSE 让父进程死时子进程跟着死。
    impl_->job = ::CreateJobObjectW(nullptr, nullptr);
    if (!impl_->job) {
        r.error = "CreateJobObject failed";
        return r;
    }
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION jeli{};
    jeli.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!::SetInformationJobObject(impl_->job, JobObjectExtendedLimitInformation,
                                   &jeli, sizeof(jeli))) {
        ::CloseHandle(impl_->job);
        impl_->job = nullptr;
        r.error = "SetInformationJobObject failed";
        return r;
    }

    // 2) 拼命令行: "exe" daemon --foreground --port=N --token=T [-dangerous]
    //    路径按 UTF-8/ACP → UTF-16 转换后再 quote,避免中文目录被按字节 widening。
    std::vector<std::wstring> argv;
    argv.push_back(exe_w);
    argv.push_back(L"daemon");
    argv.push_back(L"--foreground");
    argv.push_back(L"--port=" + std::to_wstring(req.port));
    argv.push_back(L"--token=" + acecode::utf8_to_wide(req.token));
    if (req.dangerous) argv.push_back(L"-dangerous");
    if (!req.static_dir.empty()) {
        std::wstring static_dir_w = acecode::utf8_to_wide(req.static_dir);
        if (static_dir_w.empty()) {
            r.error = "static_dir cannot be converted to UTF-16: " + req.static_dir;
            ::CloseHandle(impl_->job);
            impl_->job = nullptr;
            return r;
        }
        argv.push_back(L"--static-dir=" + static_dir_w);
    }
    if (!req.run_dir.empty()) {
        std::wstring run_dir_w = acecode::utf8_to_wide(req.run_dir);
        if (run_dir_w.empty()) {
            r.error = "run_dir cannot be converted to UTF-16: " + req.run_dir;
            ::CloseHandle(impl_->job);
            impl_->job = nullptr;
            return r;
        }
        argv.push_back(L"--run-dir=" + run_dir_w);
    }
    std::wstring cmdline_w = build_command_line_w(argv);

    // 把 stdout/stderr 重定向到 NUL。desktop 父进程是 WIN32 子系统(无 console),
    // 子进程默认继承 invalid 的 std handle,daemon 端 Logger.mirror_stderr=true 与
    // Crow 自身的 stdout 日志会反复写入 invalid handle,某些情况下阻塞 accept 循环
    // 起来,导致 desktop 端 wait_until_ready 超时(observed: workspace 'build' 报错)。
    // 用 NUL device 兜底:写 NUL 永不阻塞且永不失败。
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    HANDLE nul = ::CreateFileW(L"NUL", GENERIC_READ | GENERIC_WRITE,
                               FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                               OPEN_EXISTING, 0, nullptr);
    if (nul == INVALID_HANDLE_VALUE) nul = nullptr; // 兜底,STARTF_USESTDHANDLES 不会致命

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW | (nul ? STARTF_USESTDHANDLES : 0);
    si.wShowWindow = SW_HIDE;
    if (nul) {
        si.hStdInput  = nul;
        si.hStdOutput = nul;
        si.hStdError  = nul;
    }
    PROCESS_INFORMATION pi{};

    // CREATE_NO_WINDOW: 子进程没有 console(避免黑窗口闪一下)。
    // CREATE_SUSPENDED: 先挂起,绑 Job 后再 Resume,避免子进程创建后到绑 Job
    //   之间的窗口期里若立刻 spawn 孙子进程会逃出 Job 控制(MVP 风险低,但
    //   走这条路稳妥)。
    DWORD flags = CREATE_NO_WINDOW | CREATE_SUSPENDED;

    // lpApplicationName 显式传 exe_w,避免带空格/非 ASCII 路径时依赖系统从
    // lpCommandLine 重新解析。lpCommandLine 仍需可写 buffer 供子进程拿 argv。
    std::vector<wchar_t> cmd_buf(cmdline_w.begin(), cmdline_w.end());
    cmd_buf.push_back(L'\0');

    // lpCurrentDirectory: 让子进程在指定 cwd 启动。daemon 用 current_path()
    // 决定 session 存储目录 / project_instructions 起点等,所以这是 multi-workspace
    // 模型里关键的隔离点 — 不靠父进程改自身 cwd 的 hack。
    // bInheritHandles=TRUE 让 nul handle 真正传给子进程(STARTF_USESTDHANDLES 必需)
    BOOL ok = ::CreateProcessW(
        exe_w.c_str(),
        cmd_buf.data(),
        nullptr, nullptr,
        nul ? TRUE : FALSE, flags,
        nullptr, cwd_arg,
        &si, &pi);
    if (!ok) {
        DWORD err = ::GetLastError();
        std::ostringstream e;
        e << "CreateProcess failed (gle=" << err << ")";
        r.error = e.str();
        ::CloseHandle(impl_->job);
        impl_->job = nullptr;
        if (nul) ::CloseHandle(nul);
        return r;
    }
    // 子进程已继承 NUL handle,父端关掉自己这份引用避免 leak。
    if (nul) ::CloseHandle(nul);

    if (!::AssignProcessToJobObject(impl_->job, pi.hProcess)) {
        DWORD err = ::GetLastError();
        ::TerminateProcess(pi.hProcess, 1);
        ::CloseHandle(pi.hThread);
        ::CloseHandle(pi.hProcess);
        ::CloseHandle(impl_->job);
        impl_->job = nullptr;
        std::ostringstream e;
        e << "AssignProcessToJobObject failed (gle=" << err << ")";
        r.error = e.str();
        return r;
    }

    ::ResumeThread(pi.hThread);

    impl_->process = pi.hProcess;
    impl_->thread  = pi.hThread;
    impl_->pid     = pi.dwProcessId;

    r.ok = true;
    r.pid = static_cast<long long>(pi.dwProcessId);
    return r;
}

bool DaemonSupervisor::wait_until_ready(int port, std::chrono::milliseconds timeout) {
    auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        // 子进程已死则别傻等 — 直接失败返回。
        if (impl_->process) {
            DWORD code = 0;
            if (::GetExitCodeProcess(impl_->process, &code) && code != STILL_ACTIVE) {
                return false;
            }
        }
        if (tcp_probe(port)) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    return false;
}

void DaemonSupervisor::stop() {
    if (impl_->job) {
        ::TerminateJobObject(impl_->job, 0);
        ::CloseHandle(impl_->job);
        impl_->job = nullptr;
    }
    if (impl_->thread) {
        ::CloseHandle(impl_->thread);
        impl_->thread = nullptr;
    }
    if (impl_->process) {
        ::CloseHandle(impl_->process);
        impl_->process = nullptr;
    }
    impl_->pid = 0;
}

bool DaemonSupervisor::running() const {
    if (!impl_->process) return false;
    DWORD code = 0;
    if (!::GetExitCodeProcess(impl_->process, &code)) return false;
    return code == STILL_ACTIVE;
}

int pick_free_loopback_port() {
    SOCKET s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return 0;

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = 0; // OS 选
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    if (::bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR) {
        ::closesocket(s);
        return 0;
    }
    sockaddr_in bound{};
    int len = sizeof(bound);
    if (::getsockname(s, reinterpret_cast<sockaddr*>(&bound), &len) == SOCKET_ERROR) {
        ::closesocket(s);
        return 0;
    }
    int port = ntohs(bound.sin_port);
    ::closesocket(s);
    return port;
}

#else // !_WIN32 — POSIX stub(MVP 不支持,留接口免得 acecode_testable 链接错)

struct DaemonSupervisor::Impl {};

DaemonSupervisor::DaemonSupervisor() : impl_(new Impl()) {}
DaemonSupervisor::~DaemonSupervisor() { delete impl_; }

SpawnResult DaemonSupervisor::spawn(const SpawnRequest&) {
    SpawnResult r;
    r.error = "DaemonSupervisor: POSIX path not implemented in MVP";
    return r;
}
bool DaemonSupervisor::wait_until_ready(int, std::chrono::milliseconds) { return false; }
void DaemonSupervisor::stop() {}
bool DaemonSupervisor::running() const { return false; }
int  pick_free_loopback_port() { return 0; }

#endif

std::string make_auth_token() {
    return acecode::generate_auth_token();
}

} // namespace acecode::desktop
