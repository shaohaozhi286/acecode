// Windows PTY 后端三层实现(openspec/changes/add-console-dock,design.md D3):
//
//   ConPtyBackend  — Win10 1809+,CreatePseudoConsole(GetProcAddress 运行时
//                    探测,编译产物在老系统上也能加载)。
//   WinptyBackend  — < 1809 完整 TTY 体验。agent.exe 从嵌入字节释放到
//                    <data_dir>/bin/ 后经 set_winpty_agent_path 注入。
//   PipeBackend    — 最后兜底:匿名管道,无 TTY 语义(交互式程序不可用),
//                    backend kind 标 "pipe" 供前端显示降级提示。
//
// 线程模型(三个后端一致):
//   - 读线程:阻塞 ReadFile 循环 → on_data;管道断开后等进程退出 → on_exit。
//   - exit 顺序保证:on_exit 必然在最后一段 on_data 之后(同一线程顺序执行)。
//   - kill() 幂等:TerminateProcess + 关句柄逼断 ReadFile + join 读线程;
//     返回后不再有任何回调触发。
#ifdef _WIN32

#include "pty_backend.hpp"

#include "winpty_agent_embedded.hpp"
#include "winpty_agent_location.hpp"

#include "../../utils/encoding.hpp"
#include "../../utils/logger.hpp"
#include "../../utils/paths.hpp"
#include "../../utils/utf8_path.hpp"

#include <windows.h>
#include <winpty.h>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <thread>

namespace fs = std::filesystem;

namespace acecode {

namespace {

// ── ConPTY 动态探测 ─────────────────────────────────────────────────────
// 直接调用 CreatePseudoConsole 会让二进制在 < 1809 上加载失败,必须走
// GetProcAddress。三个函数一起解析,缺一即视为不可用。

// Older Windows SDK and MinGW header targets hide the ConPTY declarations.
// The public HPCON ABI is a void pointer, and pseudo-console is attribute 22.
using PseudoConsoleHandle = void*;
#ifdef PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE
constexpr DWORD_PTR kPseudoConsoleAttribute =
    PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE;
#else
constexpr DWORD_PTR kPseudoConsoleAttribute =
    ProcThreadAttributeValue(22, FALSE, TRUE, FALSE);
#endif

typedef HRESULT(WINAPI* CreatePseudoConsoleFn)(
    COORD, HANDLE, HANDLE, DWORD, PseudoConsoleHandle*);
typedef HRESULT(WINAPI* ResizePseudoConsoleFn)(PseudoConsoleHandle, COORD);
typedef void(WINAPI* ClosePseudoConsoleFn)(PseudoConsoleHandle);

struct ConPtyApi {
    CreatePseudoConsoleFn create = nullptr;
    ResizePseudoConsoleFn resize = nullptr;
    ClosePseudoConsoleFn close = nullptr;
    bool available() const { return create && resize && close; }
};

const ConPtyApi& conpty_api() {
    static ConPtyApi api = [] {
        ConPtyApi out;
        HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll");
        if (!kernel32) return out;
        out.create = reinterpret_cast<CreatePseudoConsoleFn>(
            GetProcAddress(kernel32, "CreatePseudoConsole"));
        out.resize = reinterpret_cast<ResizePseudoConsoleFn>(
            GetProcAddress(kernel32, "ResizePseudoConsole"));
        out.close = reinterpret_cast<ClosePseudoConsoleFn>(
            GetProcAddress(kernel32, "ClosePseudoConsole"));
        return out;
    }();
    return api;
}

std::string last_error_text(const char* what) {
    DWORD code = GetLastError();
    return std::string(what) + " failed (GetLastError=" + std::to_string(code) + ")";
}

std::string winpty_error_text(winpty_error_ptr_t err) {
    if (!err) return "(no winpty error info)";
    LPCWSTR msg = winpty_error_msg(err);
    std::string out = msg ? wide_to_utf8(msg) : "(null)";
    winpty_error_free(err);
    return out;
}

// ── winpty agent 释放 ───────────────────────────────────────────────────
// 嵌入的 agent.exe 写到 <data_dir>/bin/winpty-agent.exe。幂等:已存在且
// 字节数一致即跳过(版本随 acecode 发布更新时 size 几乎必然变化;同 size
// 不同内容的极端情况由全量重写兜底 — 这里选择 size 比对避免每次启动读
// 300KB 做哈希)。
bool ensure_winpty_agent_extracted(std::string& error) {
    static std::mutex mu;
    static bool done = false;
    std::lock_guard<std::mutex> lock(mu);
    if (done) return true;

    fs::path bin_dir = fs::path(path_from_utf8(resolve_data_dir(get_run_mode()))) / "bin";
    fs::path agent = bin_dir / "winpty-agent.exe";

    std::error_code ec;
    fs::create_directories(bin_dir, ec);
    if (ec) {
        error = "cannot create " + path_to_utf8(bin_dir) + ": " + ec.message();
        return false;
    }

    const auto embedded_size = static_cast<std::uintmax_t>(winpty_agent_size());
    if (!fs::exists(agent, ec) || fs::file_size(agent, ec) != embedded_size) {
        // 先写临时文件再 rename,避免半截 agent 被并发进程拿去执行。
        fs::path tmp = bin_dir / ("winpty-agent.tmp" + std::to_string(GetCurrentProcessId()));
        {
            std::ofstream ofs(tmp, std::ios::binary | std::ios::trunc);
            if (!ofs.is_open()) {
                error = "cannot write " + path_to_utf8(tmp);
                return false;
            }
            ofs.write(reinterpret_cast<const char*>(winpty_agent_data()),
                      static_cast<std::streamsize>(winpty_agent_size()));
        }
        fs::rename(tmp, agent, ec);
        if (ec) {
            // rename 失败但目标已存在(并发释放赢了)也算成功。
            fs::remove(tmp, ec);
            std::error_code ec2;
            if (!fs::exists(agent, ec2) || fs::file_size(agent, ec2) != embedded_size) {
                error = "cannot place " + path_to_utf8(agent);
                return false;
            }
        }
        LOG_INFO("[pty] extracted winpty-agent.exe to " + path_to_utf8(agent));
    }

    set_winpty_agent_path(agent.wstring());
    done = true;
    return true;
}

// ── 读线程公共骨架 ──────────────────────────────────────────────────────
// ReadFile(read_handle) 循环直到管道断开,然后 wait 进程、取 exit code、
// 发 on_exit。stopped_ 置位(kill())后不再触发任何回调。

class WinPtyProcessBase : public PtyProcess {
public:
    WinPtyProcessBase(PtyCallbacks callbacks) : callbacks_(std::move(callbacks)) {}

    void write(const std::string& data) override {
        std::lock_guard<std::mutex> lock(write_mu_);
        if (stopped_.load() || input_write_ == INVALID_HANDLE_VALUE) return;
        DWORD written = 0;
        WriteFile(input_write_, data.data(), static_cast<DWORD>(data.size()),
                  &written, nullptr);
    }

    void kill() override {
        bool expected = false;
        if (!stopped_.compare_exchange_strong(expected, true)) {
            join_reader();
            return;
        }
        if (process_) TerminateProcess(process_, 1);
        close_pty();  // 子类断 PTY/管道,逼读线程退出
        join_reader();
        close_handles();
    }

    int pid() const override { return static_cast<int>(pid_); }

protected:
    // 子类 spawn 成功后调用,启动读线程。
    void start_reader() {
        reader_ = std::thread([this] { reader_main(); });
    }

    void reader_main() {
        // PeekNamedPipe 轮询而非阻塞 ReadFile:ConPTY 下 ClosePseudoConsole
        // 之后输出管道并不保证断开(本机 Win11 实测 ReadFile 永远阻塞),
        // 同步 ReadFile 也无法被 CloseHandle 可靠解除。轮询统一三个后端的
        // 退出语义:管道报错(winpty/pipe 的 EOF) 或 无残余数据 + PTY 已关闭
        // (conpty close_pty 后 / kill 后)即退出。20ms 间隔对终端输出无感。
        char buf[16 * 1024];
        for (;;) {
            DWORD avail = 0;
            if (!PeekNamedPipe(output_read_, nullptr, 0, nullptr, &avail, nullptr)) {
                break;  // 管道断开(进程退出或 kill)
            }
            if (avail == 0) {
                if (stopped_.load() || pty_closed_.load()) break;  // 已关且 drain 完
                Sleep(20);
                continue;
            }
            DWORD want = static_cast<DWORD>(
                avail < sizeof(buf) ? avail : sizeof(buf));
            DWORD got = 0;
            if (!ReadFile(output_read_, buf, want, &got, nullptr) || got == 0) break;
            if (stopped_.load()) break;
            if (callbacks_.on_data) callbacks_.on_data(std::string(buf, got));
        }
        // 取退出码后通知。走到这里时进程已退出(管道断/PTY 关)或被 kill。
        DWORD exit_code = 0;
        if (process_) {
            WaitForSingleObject(process_, 10000);
            GetExitCodeProcess(process_, &exit_code);
        }
        if (!stopped_.load() && callbacks_.on_exit) {
            callbacks_.on_exit(static_cast<int>(exit_code));
        }
    }

    void join_reader() {
        if (reader_.joinable()) reader_.join();
    }

    virtual void close_pty() = 0;

    virtual void close_handles() {
        if (input_write_ != INVALID_HANDLE_VALUE) {
            CloseHandle(input_write_);
            input_write_ = INVALID_HANDLE_VALUE;
        }
        if (output_read_ != INVALID_HANDLE_VALUE) {
            CloseHandle(output_read_);
            output_read_ = INVALID_HANDLE_VALUE;
        }
        if (process_) {
            CloseHandle(process_);
            process_ = nullptr;
        }
    }

    PtyCallbacks callbacks_;
    std::atomic<bool> stopped_{false};
    // close_pty() 已执行(conpty 专用信号:管道不会断,靠它结束读循环)。
    std::atomic<bool> pty_closed_{false};
    std::mutex write_mu_;
    std::thread reader_;
    HANDLE input_write_ = INVALID_HANDLE_VALUE;  // 我们写 → shell stdin
    HANDLE output_read_ = INVALID_HANDLE_VALUE;  // shell 输出 → 我们读
    HANDLE process_ = nullptr;
    DWORD pid_ = 0;
};

// ── ConPTY ──────────────────────────────────────────────────────────────

class ConPtyProcess : public WinPtyProcessBase {
public:
    using WinPtyProcessBase::WinPtyProcessBase;

    ~ConPtyProcess() override { kill(); }

    PtyBackendKind kind() const override { return PtyBackendKind::ConPty; }

    void resize(int cols, int rows) override {
        if (stopped_.load() || !hpc_) return;
        COORD size{static_cast<SHORT>(cols), static_cast<SHORT>(rows)};
        conpty_api().resize(hpc_, size);
    }

    bool spawn(const PtySpawnSpec& spec, std::string& error) {
        const auto& api = conpty_api();

        // 两对管道:input(我们写→conpty 读) / output(conpty 写→我们读)。
        HANDLE input_read = INVALID_HANDLE_VALUE;
        HANDLE output_write = INVALID_HANDLE_VALUE;
        if (!CreatePipe(&input_read, &input_write_, nullptr, 0) ||
            !CreatePipe(&output_read_, &output_write, nullptr, 0)) {
            error = last_error_text("CreatePipe");
            return false;
        }

        COORD size{static_cast<SHORT>(spec.cols), static_cast<SHORT>(spec.rows)};
        HRESULT hr = api.create(size, input_read, output_write, 0, &hpc_);
        // conpty 持有这两端的副本,本进程的句柄立即关闭。
        CloseHandle(input_read);
        CloseHandle(output_write);
        if (FAILED(hr)) {
            error = "CreatePseudoConsole failed (hr=" + std::to_string(hr) + ")";
            return false;
        }

        // 把 HPCON 挂到子进程属性列表。
        SIZE_T attr_size = 0;
        InitializeProcThreadAttributeList(nullptr, 1, 0, &attr_size);
        attr_list_ = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(
            HeapAlloc(GetProcessHeap(), 0, attr_size));
        if (!attr_list_ ||
            !InitializeProcThreadAttributeList(attr_list_, 1, 0, &attr_size) ||
            !UpdateProcThreadAttribute(attr_list_, 0,
                                       kPseudoConsoleAttribute,
                                       hpc_, sizeof(hpc_), nullptr, nullptr)) {
            error = last_error_text("InitializeProcThreadAttributeList");
            return false;
        }

        STARTUPINFOEXW si{};
        si.StartupInfo.cb = sizeof(si);
        si.lpAttributeList = attr_list_;
        // STARTF_USESTDHANDLES + 三个 NULL 句柄(node-pty conpty 同款):
        // pseudoconsole attribute 只决定 console 绑定,std 句柄仍按老规则从父
        // 进程 PEB 复制。daemon 被 desktop 以 CREATE_NO_WINDOW + NUL std 启动
        // 时,cmd 复制到无效 stdin → 读 EOF → 立即退出 code 0(实测复现)。
        // 显式置空让 console 子进程初始化时回退绑定到 conpty console 设备。
        si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
        si.StartupInfo.hStdInput = nullptr;
        si.StartupInfo.hStdOutput = nullptr;
        si.StartupInfo.hStdError = nullptr;

        std::wstring cmdline = utf8_to_wide(spec.shell);
        std::wstring cwd = utf8_to_wide(spec.cwd);
        PROCESS_INFORMATION pi{};
        if (!CreateProcessW(nullptr, cmdline.data(), nullptr, nullptr, FALSE,
                            EXTENDED_STARTUPINFO_PRESENT, nullptr,
                            cwd.empty() ? nullptr : cwd.c_str(),
                            &si.StartupInfo, &pi)) {
            error = last_error_text("CreateProcessW");
            return false;
        }
        CloseHandle(pi.hThread);
        process_ = pi.hProcess;
        pid_ = pi.dwProcessId;

        // 进程退出不会自动断 output 管道(conhost 持有写端)。等待线程在
        // 进程退出后 ClosePseudoConsole(同步 flush 尾部输出到管道),再置
        // pty_closed_ — 读线程 drain 完残余后据此结束(见 reader_main 注释)。
        waiter_ = std::thread([this] {
            WaitForSingleObject(process_, INFINITE);
            close_pty();
            pty_closed_.store(true);
        });

        start_reader();
        return true;
    }

    void kill() override {
        WinPtyProcessBase::kill();
        if (waiter_.joinable()) waiter_.join();
        if (attr_list_) {
            DeleteProcThreadAttributeList(attr_list_);
            HeapFree(GetProcessHeap(), 0, attr_list_);
            attr_list_ = nullptr;
        }
    }

protected:
    void close_pty() override {
        std::lock_guard<std::mutex> lock(hpc_mu_);
        if (hpc_) {
            conpty_api().close(hpc_);
            hpc_ = nullptr;
        }
    }

private:
    PseudoConsoleHandle hpc_ = nullptr;
    std::mutex hpc_mu_;
    LPPROC_THREAD_ATTRIBUTE_LIST attr_list_ = nullptr;
    std::thread waiter_;
};

// ── winpty ──────────────────────────────────────────────────────────────

class WinptyProcess : public WinPtyProcessBase {
public:
    using WinPtyProcessBase::WinPtyProcessBase;

    ~WinptyProcess() override { kill(); }

    PtyBackendKind kind() const override { return PtyBackendKind::Winpty; }

    void resize(int cols, int rows) override {
        std::lock_guard<std::mutex> lock(wp_mu_);
        if (stopped_.load() || !wp_) return;
        winpty_error_ptr_t err = nullptr;
        if (!winpty_set_size(wp_, cols, rows, &err)) {
            LOG_WARN("[pty] winpty_set_size failed: " + winpty_error_text(err));
        }
    }

    bool spawn(const PtySpawnSpec& spec, std::string& error) {
        if (!ensure_winpty_agent_extracted(error)) return false;

        winpty_error_ptr_t err = nullptr;
        winpty_config_t* cfg = winpty_config_new(0, &err);
        if (!cfg) {
            error = "winpty_config_new: " + winpty_error_text(err);
            return false;
        }
        winpty_config_set_initial_size(cfg, spec.cols, spec.rows);
        wp_ = winpty_open(cfg, &err);
        winpty_config_free(cfg);
        if (!wp_) {
            error = "winpty_open: " + winpty_error_text(err);
            return false;
        }

        input_write_ = CreateFileW(winpty_conin_name(wp_), GENERIC_WRITE, 0,
                                   nullptr, OPEN_EXISTING, 0, nullptr);
        output_read_ = CreateFileW(winpty_conout_name(wp_), GENERIC_READ, 0,
                                   nullptr, OPEN_EXISTING, 0, nullptr);
        if (input_write_ == INVALID_HANDLE_VALUE ||
            output_read_ == INVALID_HANDLE_VALUE) {
            error = last_error_text("CreateFileW(winpty pipes)");
            return false;
        }

        std::wstring shell = utf8_to_wide(spec.shell);
        std::wstring cwd = utf8_to_wide(spec.cwd);
        // appname=nullptr + cmdline=shell:让 winpty 走 PATH 解析,支持
        // console.shell 配置裸命令名("pwsh")而不仅是绝对路径。
        winpty_spawn_config_t* spawn_cfg = winpty_spawn_config_new(
            WINPTY_SPAWN_FLAG_AUTO_SHUTDOWN, nullptr, shell.c_str(),
            cwd.empty() ? nullptr : cwd.c_str(), nullptr, &err);
        if (!spawn_cfg) {
            error = "winpty_spawn_config_new: " + winpty_error_text(err);
            return false;
        }
        BOOL spawned = winpty_spawn(wp_, spawn_cfg, &process_, nullptr, nullptr, &err);
        winpty_spawn_config_free(spawn_cfg);
        if (!spawned) {
            error = "winpty_spawn: " + winpty_error_text(err);
            return false;
        }
        pid_ = GetProcessId(process_);

        // AUTO_SHUTDOWN:子进程退出后 agent 跟随关闭,conout 管道 EOF,
        // 读线程自然走到 on_exit,无需额外等待线程。
        start_reader();
        return true;
    }

protected:
    void close_pty() override {
        std::lock_guard<std::mutex> lock(wp_mu_);
        if (wp_) {
            winpty_free(wp_);  // 关 agent,断管道
            wp_ = nullptr;
        }
    }

private:
    winpty_t* wp_ = nullptr;
    std::mutex wp_mu_;
};

// ── 管道兜底 ────────────────────────────────────────────────────────────

class PipeProcess : public WinPtyProcessBase {
public:
    using WinPtyProcessBase::WinPtyProcessBase;

    ~PipeProcess() override { kill(); }

    PtyBackendKind kind() const override { return PtyBackendKind::Pipe; }

    void resize(int, int) override {}  // 无 TTY,无尺寸概念

    bool spawn(const PtySpawnSpec& spec, std::string& error) {
        SECURITY_ATTRIBUTES sa{};
        sa.nLength = sizeof(sa);
        sa.bInheritHandle = TRUE;

        HANDLE input_read = INVALID_HANDLE_VALUE;
        HANDLE output_write = INVALID_HANDLE_VALUE;
        if (!CreatePipe(&input_read, &input_write_, &sa, 0) ||
            !CreatePipe(&output_read_, &output_write, &sa, 0)) {
            error = last_error_text("CreatePipe");
            return false;
        }
        // 我们持有的两端不继承给子进程。
        SetHandleInformation(input_write_, HANDLE_FLAG_INHERIT, 0);
        SetHandleInformation(output_read_, HANDLE_FLAG_INHERIT, 0);

        STARTUPINFOW si{};
        si.cb = sizeof(si);
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdInput = input_read;
        si.hStdOutput = output_write;
        si.hStdError = output_write;  // stderr 合流

        std::wstring cmdline = utf8_to_wide(spec.shell);
        std::wstring cwd = utf8_to_wide(spec.cwd);
        PROCESS_INFORMATION pi{};
        BOOL ok = CreateProcessW(nullptr, cmdline.data(), nullptr, nullptr,
                                 TRUE, CREATE_NO_WINDOW, nullptr,
                                 cwd.empty() ? nullptr : cwd.c_str(), &si, &pi);
        // 子进程已持有副本(或创建失败),父进程侧立即关闭。
        CloseHandle(input_read);
        CloseHandle(output_write);
        if (!ok) {
            error = last_error_text("CreateProcessW");
            return false;
        }
        CloseHandle(pi.hThread);
        process_ = pi.hProcess;
        pid_ = pi.dwProcessId;

        start_reader();
        return true;
    }

protected:
    void close_pty() override {
        // 子端句柄已关;Terminate 后输出管道写端无人持有,读线程自然 EOF。
    }
};

} // namespace

PtyBackendKind detect_pty_backend() {
    if (conpty_api().available()) return PtyBackendKind::ConPty;
    // winpty 可用性 = agent 可释放 + winpty_open 能跑。释放失败(磁盘/权限)
    // 在 spawn 时才暴露;探测阶段只看代码路径是否存在 — winpty 静态链入,
    // 永远存在,因此非 ConPTY 一律先报 winpty,spawn 失败再降 pipe。
    return PtyBackendKind::Winpty;
}

std::unique_ptr<PtyProcess> spawn_pty(PtyBackendKind kind,
                                      const PtySpawnSpec& spec,
                                      PtyCallbacks callbacks,
                                      std::string& error) {
    switch (kind) {
        case PtyBackendKind::ConPty: {
            if (!conpty_api().available()) {
                error = "ConPTY is not available on this system";
                return nullptr;
            }
            auto p = std::make_unique<ConPtyProcess>(std::move(callbacks));
            if (!p->spawn(spec, error)) return nullptr;
            return p;
        }
        case PtyBackendKind::Winpty: {
            auto p = std::make_unique<WinptyProcess>(std::move(callbacks));
            if (!p->spawn(spec, error)) return nullptr;
            return p;
        }
        case PtyBackendKind::Pipe: {
            auto p = std::make_unique<PipeProcess>(std::move(callbacks));
            if (!p->spawn(spec, error)) return nullptr;
            return p;
        }
        case PtyBackendKind::PosixPty:
            error = "posix backend is not available on Windows";
            return nullptr;
    }
    error = "unknown pty backend";
    return nullptr;
}

} // namespace acecode

#endif // _WIN32
