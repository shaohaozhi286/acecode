#include "web_host.hpp"

#include "../utils/logger.hpp"

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#endif

#include <webview/webview.h>

#include <algorithm>
#include <memory>
#include <mutex>

#ifdef _WIN32
#  include <windows.h>
#endif

namespace acecode::desktop {

#ifdef _WIN32
namespace {

constexpr wchar_t kHostWindowClassName[] = L"ACECodeDesktopHostWindow";

HMONITOR active_monitor() {
    if (HWND fg = ::GetForegroundWindow()) {
        if (HMONITOR monitor = ::MonitorFromWindow(fg, MONITOR_DEFAULTTONEAREST)) {
            return monitor;
        }
    }
    POINT pt{};
    if (::GetCursorPos(&pt)) {
        if (HMONITOR monitor = ::MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST)) {
            return monitor;
        }
    }
    return ::MonitorFromWindow(nullptr, MONITOR_DEFAULTTOPRIMARY);
}

RECT active_monitor_work_rect() {
    RECT fallback{
        0,
        0,
        ::GetSystemMetrics(SM_CXSCREEN),
        ::GetSystemMetrics(SM_CYSCREEN),
    };
    HMONITOR monitor = active_monitor();
    if (!monitor) return fallback;

    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    if (!::GetMonitorInfoW(monitor, &mi)) return fallback;
    return mi.rcWork;
}

void resize_webview_widget(HWND hwnd) {
    HWND widget = ::FindWindowExW(hwnd, nullptr, L"webview_widget", nullptr);
    if (!widget) return;

    RECT client{};
    if (!::GetClientRect(hwnd, &client)) return;
    ::MoveWindow(widget, 0, 0, client.right - client.left, client.bottom - client.top, TRUE);
}

LRESULT CALLBACK host_window_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    switch (msg) {
        case WM_SIZE:
            resize_webview_widget(hwnd);
            break;
        case WM_CLOSE:
            ::DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            ::PostQuitMessage(0);
            return 0;
        default:
            break;
    }
    return ::DefWindowProcW(hwnd, msg, wparam, lparam);
}

bool register_host_window_class(HINSTANCE instance) {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.hInstance = instance;
    wc.lpszClassName = kHostWindowClassName;
    wc.lpfnWndProc = host_window_proc;
    wc.hCursor = ::LoadCursorW(nullptr, MAKEINTRESOURCEW(32512)); // IDC_ARROW
    if (::RegisterClassExW(&wc)) return true;
    return ::GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
}

HWND create_offscreen_host_window(RECT& target_monitor) {
    HINSTANCE instance = ::GetModuleHandleW(nullptr);
    if (!register_host_window_class(instance)) return nullptr;

    target_monitor = active_monitor_work_rect();
    const int x = target_monitor.right + 10000;
    const int y = target_monitor.bottom + 10000;
    HWND hwnd = ::CreateWindowExW(
        WS_EX_APPWINDOW,
        kHostWindowClassName,
        L"ACECode",
        WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
        x,
        y,
        1280,
        820,
        nullptr,
        nullptr,
        instance,
        nullptr);
    if (hwnd) {
        // WebView2 initialization for an externally-owned parent HWND is more
        // reliable when the parent is already visible. It is offscreen here,
        // so this does not expose a blank window to the user.
        ::ShowWindow(hwnd, SW_SHOWNOACTIVATE);
        ::UpdateWindow(hwnd);
    }
    return hwnd;
}

void center_window_on_monitor(HWND hwnd, const RECT& monitor) {
    RECT rect{};
    if (!::GetWindowRect(hwnd, &rect)) return;

    const int w = rect.right - rect.left;
    const int h = rect.bottom - rect.top;
    const int monitor_w = monitor.right - monitor.left;
    const int monitor_h = monitor.bottom - monitor.top;
    const int x = monitor.left + std::max(0, (monitor_w - w) / 2);
    const int y = monitor.top + std::max(0, (monitor_h - h) / 2);
    ::SetWindowPos(hwnd, nullptr, x, y, w, h, SWP_NOZORDER | SWP_NOACTIVATE);
}

} // namespace

struct ComApartment {
    explicit ComApartment(bool enable) {
        if (!enable) return;
        HRESULT hr = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        should_uninitialize = (hr == S_OK || hr == S_FALSE);
    }

    ~ComApartment() {
        if (should_uninitialize) {
            ::CoUninitialize();
        }
    }

    ComApartment(const ComApartment&) = delete;
    ComApartment& operator=(const ComApartment&) = delete;

    bool should_uninitialize = false;
};
#endif

struct WebHost::Impl {
    explicit Impl(bool debug, StartupWindowMode startup_mode)
#ifdef _WIN32
        : custom_window(startup_mode == StartupWindowMode::OffscreenUntilReady
                            ? create_offscreen_host_window(target_monitor)
                            : nullptr),
          offscreen_until_ready(custom_window != nullptr),
          com(custom_window != nullptr) {
        try {
            w = std::make_unique<webview::webview>(
                debug,
                custom_window ? static_cast<void*>(custom_window) : nullptr);
        } catch (const webview::exception& e) {
            if (!custom_window) throw;
            LOG_WARN(std::string("[desktop] offscreen WebView host failed; falling back: ") +
                     e.what());
            if (::IsWindow(custom_window)) {
                ::DestroyWindow(custom_window);
            }
            custom_window = nullptr;
            offscreen_until_ready = false;
            w = std::make_unique<webview::webview>(debug, nullptr);
        }
        if (custom_window) {
            resize_webview_widget(custom_window);
        }
    }
#else
    {
        (void)startup_mode;
        w = std::make_unique<webview::webview>(debug, nullptr);
    }
#endif

    ~Impl() {
#ifdef _WIN32
        HWND hwnd = custom_window;
#endif
        // Destroy webview first; for m_owns_window=false it removes only the child widget.
        // The parent HWND remains ours and is destroyed below.
        w.reset();
#ifdef _WIN32
        if (hwnd && ::IsWindow(hwnd)) {
            ::DestroyWindow(hwnd);
        }
#endif
    }

#ifdef _WIN32
    RECT target_monitor{};
    HWND custom_window = nullptr;
    bool offscreen_until_ready = false;
    ComApartment com{false};
#endif
    std::unique_ptr<webview::webview> w;
};

WebHost::WebHost(bool debug, StartupWindowMode startup_mode)
    : impl_(new Impl(debug, startup_mode)) {}
WebHost::~WebHost() { delete impl_; }

void WebHost::set_title(const std::string& title) {
    impl_->w->set_title(title);
}
void WebHost::set_size(int width, int height) {
    impl_->w->set_size(width, height, WEBVIEW_HINT_NONE);
#ifdef _WIN32
    if (impl_->custom_window) {
        resize_webview_widget(impl_->custom_window);
    }
#endif
}
void WebHost::navigate(const std::string& url) {
    impl_->w->navigate(url);
}
void WebHost::set_visible(bool visible) {
#ifdef _WIN32
    auto r = impl_->w->window();
    if (!r.ok()) return;
    HWND hwnd = static_cast<HWND>(r.value());
    if (!hwnd) return;
    if (visible && impl_->offscreen_until_ready) {
        center_window_on_monitor(hwnd, impl_->target_monitor);
        impl_->offscreen_until_ready = false;
    }
    ::ShowWindow(hwnd, visible ? SW_SHOW : SW_HIDE);
    if (visible) {
        ::UpdateWindow(hwnd);
        ::SetForegroundWindow(hwnd);
    }
#else
    (void)visible;
#endif
}
void WebHost::init_script(const std::string& js) {
    impl_->w->init(js);
}
void WebHost::eval(const std::string& js) {
    // webview C++ 的 eval 内部最终走平台 InvokeScript 调用,不一定保证跨线程
    // 安全。dispatch 把 eval 调度到 webview 主循环线程,跨线程 caller 安全。
    auto js_copy = js;
    auto* w_ptr = impl_->w.get();
    impl_->w->dispatch([w_ptr, js_copy] {
        w_ptr->eval(js_copy);
    });
}
bool WebHost::open_dev_tools() {
#ifdef _WIN32
    auto controller_result = impl_->w->browser_controller();
    if (!controller_result.ok()) return false;
    auto* controller = static_cast<ICoreWebView2Controller*>(controller_result.value());
    if (!controller) return false;

    ICoreWebView2* webview = nullptr;
    HRESULT hr = controller->get_CoreWebView2(&webview);
    if (FAILED(hr) || !webview) return false;
    hr = webview->OpenDevToolsWindow();
    webview->Release();
    return SUCCEEDED(hr);
#else
    return false;
#endif
}
void WebHost::bind(const std::string& name, SyncHandler fn) {
    impl_->w->bind(name, [fn](const std::string& req) -> std::string {
        return fn(req);
    });
}
void WebHost::run() {
    impl_->w->run();
}

void* WebHost::native_window() const {
    // basic_result<void*>: ok()/value() 接口。失败时返回 nullptr 让 folder picker
    // 用 NULL parent(对 IFileOpenDialog 是合法的,弹窗 modal 关系会缺失)。
    auto r = impl_->w->window();
    return r.ok() ? r.value() : nullptr;
}

} // namespace acecode::desktop
