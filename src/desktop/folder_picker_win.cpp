#include "folder_picker.hpp"

#ifdef _WIN32

#include "../utils/logger.hpp"

#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#include <windows.h>
#include <shobjidl.h>
#include <shlobj.h>

#include <atomic>
#include <chrono>
#include <cwchar>
#include <exception>
#include <string>
#include <thread>

namespace acecode::desktop {

namespace {

constexpr const wchar_t* kFolderPickerTitle = L"Select project folder";

// UTF-16 → UTF-8。基础实现,夹带不可解码字节直接落空(MVP 容忍,实际选目录路径都是
// 系统合法 UTF-16)。
std::string wide_to_utf8(const std::wstring& w) {
    if (w.empty()) return {};
    int len = ::WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()),
                                    nullptr, 0, nullptr, nullptr);
    if (len <= 0) return {};
    std::string out(static_cast<size_t>(len), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()),
                          out.data(), len, nullptr, nullptr);
    return out;
}

bool is_usable_owner_window(HWND hwnd) {
    if (!hwnd || !::IsWindow(hwnd) || !::IsWindowVisible(hwnd)) return false;
    LONG_PTR style = ::GetWindowLongPtrW(hwnd, GWL_STYLE);
    return (style & WS_VISIBLE) != 0;
}

HWND resolve_dialog_owner(HWND requested_parent) {
    HWND owner = requested_parent;
    if (!owner) {
        // Edge app compatibility mode calls the daemon endpoint, so the daemon
        // has no WebView HWND. The user's click still leaves the Edge app as the
        // foreground window; using it as owner keeps the dialog above that app.
        owner = ::GetForegroundWindow();
    }
    if (owner) {
        if (HWND root = ::GetAncestor(owner, GA_ROOT); root) {
            owner = root;
        }
    }
    return is_usable_owner_window(owner) ? owner : nullptr;
}

struct DialogForegroundState {
    DWORD pid = 0;
    bool found = false;
};

// 既认标题也认窗口类:IFileDialog 的顶层窗口类固定是 #32770(通用对话框),
// SetTitle 之后标题应当匹配,但个别 Windows 版本上标题落地有时序窗口,曾导致
// 仅按标题精确匹配的脉冲整轮扫不到对话框(webapp 模式下对话框永远垫底)。
// daemon 进程没有其它可见顶层窗口,pid + #32770 足够准确。
bool is_folder_picker_window(HWND hwnd) {
    wchar_t title[128] = {0};
    ::GetWindowTextW(hwnd, title, static_cast<int>(sizeof(title) / sizeof(title[0])));
    if (std::wcscmp(title, kFolderPickerTitle) == 0) return true;

    wchar_t cls[64] = {0};
    ::GetClassNameW(hwnd, cls, static_cast<int>(sizeof(cls) / sizeof(cls[0])));
    return std::wcscmp(cls, L"#32770") == 0;
}

// 后台进程直接 SetForegroundWindow 大概率被拒(foreground lock)。经典缓解是把
// 当前线程的输入队列 attach 到前台线程后再调用 —— 此时系统认为调用方"参与"了
// 前台输入,放行率显著更高(AutoHotkey / 各类 launcher 的常用组合)。失败再退回
// 裸调用,保底仍有下方的 TOPMOST 脉冲保证 z-order 可见。
void force_foreground(HWND hwnd) {
    const DWORD my_tid = ::GetCurrentThreadId();
    const DWORD fg_tid = ::GetWindowThreadProcessId(::GetForegroundWindow(), nullptr);
    bool attached = false;
    if (fg_tid && fg_tid != my_tid) {
        attached = ::AttachThreadInput(my_tid, fg_tid, TRUE) != 0;
    }
    ::BringWindowToTop(hwnd);
    ::SetForegroundWindow(hwnd);
    if (attached) {
        ::AttachThreadInput(my_tid, fg_tid, FALSE);
    }
}

BOOL CALLBACK pulse_folder_dialog_proc(HWND hwnd, LPARAM param) {
    auto* state = reinterpret_cast<DialogForegroundState*>(param);
    if (!state || !hwnd || !::IsWindowVisible(hwnd)) return TRUE;

    DWORD pid = 0;
    ::GetWindowThreadProcessId(hwnd, &pid);
    if (pid != state->pid) return TRUE;

    if (!is_folder_picker_window(hwnd)) return TRUE;

    if (::IsIconic(hwnd)) {
        ::ShowWindow(hwnd, SW_RESTORE);
    } else {
        ::ShowWindow(hwnd, SW_SHOW);
    }

    // Windows can deny SetForegroundWindow for background processes. A brief
    // topmost pulse still makes the user-initiated dialog visible, then returns
    // it to normal z-order so it does not stay globally topmost.
    constexpr UINT flags = SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW;
    ::SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, flags);
    ::SetWindowPos(hwnd, HWND_NOTOPMOST, 0, 0, 0, 0, flags);
    force_foreground(hwnd);
    state->found = true;
    return TRUE;
}

class DialogForegroundPulse {
public:
    DialogForegroundPulse() = default;
    DialogForegroundPulse(const DialogForegroundPulse&) = delete;
    DialogForegroundPulse& operator=(const DialogForegroundPulse&) = delete;

    ~DialogForegroundPulse() { stop(); }

    void start() {
        stop_.store(false);
        try {
            worker_ = std::thread([this] {
                const DWORD pid = ::GetCurrentProcessId();
                const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
                while (!stop_.load() && std::chrono::steady_clock::now() < deadline) {
                    DialogForegroundState state;
                    state.pid = pid;
                    ::EnumWindows(pulse_folder_dialog_proc, reinterpret_cast<LPARAM>(&state));
                    if (state.found) {
                        // One or two extra pulses cover the common-dialog activation
                        // race without fighting the user for focus afterward.
                        for (int i = 0; i < 2 && !stop_.load(); ++i) {
                            std::this_thread::sleep_for(std::chrono::milliseconds(80));
                            DialogForegroundState again;
                            again.pid = pid;
                            ::EnumWindows(pulse_folder_dialog_proc, reinterpret_cast<LPARAM>(&again));
                        }
                        break;
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(80));
                }
            });
        } catch (const std::exception& e) {
            LOG_WARN(std::string("[folder_picker] foreground pulse unavailable: ") + e.what());
        }
    }

    void stop() {
        stop_.store(true);
        if (worker_.joinable()) worker_.join();
    }

private:
    std::atomic<bool> stop_{false};
    std::thread worker_;
};

} // namespace

std::optional<std::string> pick_folder(void* parent_hwnd) {
    // CoInitializeEx: STA 模式;若已 init 过(webview 内部已初始化 COM)
    // 返回 RPC_E_CHANGED_MODE,这种情况直接当 OK 处理 — IFileOpenDialog 在 STA 用得正常。
    HRESULT hr_init = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    bool we_init_com = SUCCEEDED(hr_init);
    if (!we_init_com && hr_init != RPC_E_CHANGED_MODE && hr_init != S_FALSE) {
        LOG_WARN("[folder_picker] CoInitializeEx failed");
        return std::nullopt;
    }

    std::optional<std::string> result;

    IFileOpenDialog* dlg = nullptr;
    HRESULT hr = ::CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                    IID_PPV_ARGS(&dlg));
    if (SUCCEEDED(hr) && dlg) {
        // FOS_PICKFOLDERS: 选目录而不是文件;FOS_FORCEFILESYSTEM: 强制返回真实磁盘路径,
        // 排除虚拟 namespace(虚拟回收站之类)。
        DWORD opts = 0;
        if (SUCCEEDED(dlg->GetOptions(&opts))) {
            dlg->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
        }
        dlg->SetTitle(kFolderPickerTitle);

        HWND requested_parent = reinterpret_cast<HWND>(parent_hwnd);
        HWND parent = resolve_dialog_owner(requested_parent);
        DialogForegroundPulse foreground_pulse;
        if (!requested_parent || !parent) {
            foreground_pulse.start();
        }
        hr = dlg->Show(parent);
        foreground_pulse.stop();
        if (SUCCEEDED(hr)) {
            IShellItem* item = nullptr;
            if (SUCCEEDED(dlg->GetResult(&item)) && item) {
                PWSTR path = nullptr;
                if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) && path) {
                    result = wide_to_utf8(std::wstring(path));
                    ::CoTaskMemFree(path);
                }
                item->Release();
            }
        }
        // hr 可能是 HRESULT_FROM_WIN32(ERROR_CANCELLED) — 用户取消,不报错
        dlg->Release();
    } else {
        LOG_WARN("[folder_picker] CoCreateInstance(IFileOpenDialog) failed");
    }

    if (we_init_com) ::CoUninitialize();
    return result;
}

} // namespace acecode::desktop

#elif !defined(__APPLE__) // POSIX stub (Linux etc.)

#include "../utils/logger.hpp"

#include <array>
#include <cerrno>
#include <cstring>
#include <optional>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

namespace acecode::desktop {

namespace {

std::optional<std::string> run_folder_picker_command(const std::vector<const char*>& argv) {
    int pipefd[2] = {-1, -1};
    if (::pipe(pipefd) != 0) {
        LOG_WARN(std::string("[folder_picker] pipe failed: ") + std::strerror(errno));
        return std::nullopt;
    }

    pid_t pid = ::fork();
    if (pid < 0) {
        LOG_WARN(std::string("[folder_picker] fork failed: ") + std::strerror(errno));
        ::close(pipefd[0]);
        ::close(pipefd[1]);
        return std::nullopt;
    }

    if (pid == 0) {
        ::close(pipefd[0]);
        ::dup2(pipefd[1], STDOUT_FILENO);
        ::close(pipefd[1]);
        ::execvp(argv[0], const_cast<char* const*>(argv.data()));
        ::_exit(errno == ENOENT ? 127 : 126);
    }

    ::close(pipefd[1]);
    std::string out;
    std::array<char, 512> buf{};
    while (true) {
        ssize_t n = ::read(pipefd[0], buf.data(), buf.size());
        if (n > 0) {
            out.append(buf.data(), static_cast<size_t>(n));
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        break;
    }
    ::close(pipefd[0]);

    int status = 0;
    while (::waitpid(pid, &status, 0) < 0) {
        if (errno == EINTR) continue;
        LOG_WARN(std::string("[folder_picker] waitpid failed: ") + std::strerror(errno));
        return std::nullopt;
    }

    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        return std::nullopt;
    }
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) {
        out.pop_back();
    }
    if (out.empty()) return std::nullopt;
    return out;
}

} // namespace

std::optional<std::string> pick_folder(void* /*parent*/) {
    // Linux best effort without adding a hard GTK dependency to acecode_testable.
    // Desktop packages can depend on zenity/kdialog for a native folder dialog;
    // absence or user cancellation both map to nullopt.
    if (auto picked = run_folder_picker_command({
            "zenity",
            "--file-selection",
            "--directory",
            "--title=Select project folder",
            nullptr,
        })) {
        return picked;
    }
    if (auto picked = run_folder_picker_command({
            "kdialog",
            "--getexistingdirectory",
            ".",
            "--title",
            "Select project folder",
            nullptr,
        })) {
        return picked;
    }
    return std::nullopt;
}

} // namespace acecode::desktop

#endif
