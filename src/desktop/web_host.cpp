#include "web_host.hpp"

#include "web_host_close_policy.hpp"
#include "webview2_runtime_probe.hpp"
#include "window_chrome.hpp"

#include "../utils/logger.hpp"

#include <functional>

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
#include <utility>

#ifdef _WIN32
#  include <windows.h>
#endif
#if !defined(_WIN32) && !defined(__APPLE__)
#  include <dlfcn.h>
#endif
#ifdef __APPLE__
#  include <CoreGraphics/CoreGraphics.h>
#endif

namespace acecode::desktop {

namespace {

// 全局 close_request_handler — 只在主线程上写,窗口回调在同一 GUI 主线程上读。
std::function<bool()> g_close_handler;

} // namespace

#ifdef _WIN32
namespace {

constexpr wchar_t kHostWindowClassName[] = L"ACECodeDesktopHostWindow";
constexpr int kFramelessDragHeightDip = 44;

// WM_USER 区私有消息:绕过 close_request_handler 直接走 DestroyWindow。
// 选 0x10 偏移留出 0..0xF 给未来扩展;远离 webview/Common Controls 常用的
// WM_USER..WM_USER+0x100 区段。
constexpr UINT kRequestQuitMsg = WM_USER + 0x10;

// 窗口最大化/还原状态变化 handler。同样 main thread only。WndProc 在 WM_SIZE
// 时检测 IsZoomed 与 g_last_known_maximized 是否不同,变化时触发并同步缓存。
// 缓存初始 false,WM_SIZE 第一次到达时若为 maximized 会被推送一次,frontend
// 也照样会通过 aceDesktop_isWindowMaximized 拿初始态;两者最终一致。
std::function<void(bool)> g_window_state_handler;
bool g_last_known_maximized = false;

// 单例联动:第二次启动 acecode-desktop 会向已有 host window 派 focus msg,
// 让我们把窗口拉前 + 显示。同名 RegisterWindowMessageW 在两端拿到一致 UINT,
// 见 single_instance_win.cpp 头注。第一次访问时 lazily register,缓存到 static。
UINT focus_existing_instance_msg() {
    static UINT id = ::RegisterWindowMessageW(L"ACECode_FocusExistingInstance_v1");
    return id;
}

int dpi_scale(int value, UINT dpi) {
    return static_cast<int>((static_cast<long long>(value) * static_cast<long long>(dpi)) / 96);
}

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

void refresh_non_client_frame(HWND hwnd) {
    RECT rect{};
    if (!::GetWindowRect(hwnd, &rect)) return;
    ::SetWindowPos(hwnd,
                   nullptr,
                   rect.left,
                   rect.top,
                   rect.right - rect.left,
                   rect.bottom - rect.top,
                   SWP_FRAMECHANGED | SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOMOVE |
                       SWP_NOSIZE);
}

void log_webview_setting_failure(const char* operation, HRESULT hr) {
    LOG_WARN(std::string("[desktop] ") + operation +
             " failed, hr=" + std::to_string(static_cast<long>(hr)));
}

void configure_browser_defaults(webview::webview& host) {
    auto controller_result = host.browser_controller();
    if (!controller_result.ok()) {
        LOG_WARN("[desktop] browser_controller unavailable; browser defaults not configured");
        return;
    }
    auto* controller = static_cast<ICoreWebView2Controller*>(controller_result.value());
    if (!controller) return;

    HRESULT hr = controller->put_ZoomFactor(1.0);
    if (FAILED(hr)) {
        log_webview_setting_failure("put_ZoomFactor", hr);
    }

    ICoreWebView2* core = nullptr;
    hr = controller->get_CoreWebView2(&core);
    if (FAILED(hr) || !core) {
        log_webview_setting_failure("get_CoreWebView2", hr);
        return;
    }

    ICoreWebView2Settings* settings = nullptr;
    hr = core->get_Settings(&settings);
    core->Release();
    if (FAILED(hr) || !settings) {
        log_webview_setting_failure("get_Settings", hr);
        return;
    }

    hr = settings->put_IsZoomControlEnabled(FALSE);
    if (FAILED(hr)) {
        log_webview_setting_failure("put_IsZoomControlEnabled", hr);
    }

    ICoreWebView2Settings5* settings5 = nullptr;
    hr = settings->QueryInterface(IID_PPV_ARGS(&settings5));
    if (SUCCEEDED(hr) && settings5) {
        const HRESULT pinch_hr = settings5->put_IsPinchZoomEnabled(FALSE);
        if (FAILED(pinch_hr)) {
            log_webview_setting_failure("put_IsPinchZoomEnabled", pinch_hr);
        }
        settings5->Release();
    }

    ICoreWebView2Settings6* settings6 = nullptr;
    hr = settings->QueryInterface(IID_PPV_ARGS(&settings6));
    if (SUCCEEDED(hr) && settings6) {
        const HRESULT swipe_hr = settings6->put_IsSwipeNavigationEnabled(FALSE);
        if (FAILED(swipe_hr)) {
            log_webview_setting_failure("put_IsSwipeNavigationEnabled", swipe_hr);
        }
        settings6->Release();
    }

    settings->Release();
}

int win32_hit_test_value(FramelessHitTestArea area) {
    switch (area) {
        // Keep WebView top-bar controls clickable. Dragging is started by the
        // React top bar through aceDesktop_startWindowDrag, so the parent HWND
        // should expose the caption area as client unless it is a resize edge.
        case FramelessHitTestArea::Caption: return HTCLIENT;
        case FramelessHitTestArea::Left: return HTLEFT;
        case FramelessHitTestArea::Right: return HTRIGHT;
        case FramelessHitTestArea::Top: return HTTOP;
        case FramelessHitTestArea::TopLeft: return HTTOPLEFT;
        case FramelessHitTestArea::TopRight: return HTTOPRIGHT;
        case FramelessHitTestArea::Bottom: return HTBOTTOM;
        case FramelessHitTestArea::BottomLeft: return HTBOTTOMLEFT;
        case FramelessHitTestArea::BottomRight: return HTBOTTOMRIGHT;
        case FramelessHitTestArea::Client:
        default: return HTCLIENT;
    }
}

LRESULT frameless_hit_test(HWND hwnd, LPARAM lparam) {
    RECT window{};
    if (!::GetWindowRect(hwnd, &window)) return HTCLIENT;

    const int screen_x = static_cast<int>(static_cast<short>(LOWORD(lparam)));
    const int screen_y = static_cast<int>(static_cast<short>(HIWORD(lparam)));
    const UINT dpi = ::GetDpiForWindow(hwnd);
    FramelessHitTestInput input;
    input.x = screen_x - window.left;
    input.y = screen_y - window.top;
    input.width = window.right - window.left;
    input.height = window.bottom - window.top;
    input.frame_x = ::GetSystemMetricsForDpi(SM_CXFRAME, dpi);
    input.frame_y = ::GetSystemMetricsForDpi(SM_CYFRAME, dpi);
    input.padding = ::GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi);
    input.drag_height = dpi_scale(kFramelessDragHeightDip, dpi);
    input.maximized = ::IsZoomed(hwnd) != FALSE;
    return win32_hit_test_value(classify_frameless_hit_test(input));
}

LRESULT frameless_nc_calc(HWND hwnd, WPARAM wparam, LPARAM lparam) {
    if (!wparam || !lparam) return ::DefWindowProcW(hwnd, WM_NCCALCSIZE, wparam, lparam);

    const UINT dpi = ::GetDpiForWindow(hwnd);
    const int frame_x = ::GetSystemMetricsForDpi(SM_CXFRAME, dpi);
    const int frame_y = ::GetSystemMetricsForDpi(SM_CYFRAME, dpi);
    const int padding = ::GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi);

    auto* params = reinterpret_cast<NCCALCSIZE_PARAMS*>(lparam);
    RECT& client = params->rgrc[0];
    client.left += frame_x + padding;
    client.right -= frame_x + padding;
    client.bottom -= frame_y + padding;
    if (::IsZoomed(hwnd)) {
        client.top += padding;
    }
    return 0;
}

LRESULT CALLBACK host_window_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    switch (msg) {
        case WM_NCCALCSIZE:
            return frameless_nc_calc(hwnd, wparam, lparam);
        case WM_NCHITTEST:
            return frameless_hit_test(hwnd, lparam);
        case WM_SIZE:
            resize_webview_widget(hwnd);
            if (g_window_state_handler) {
                const bool maximized = ::IsZoomed(hwnd) != FALSE;
                if (maximized != g_last_known_maximized) {
                    g_last_known_maximized = maximized;
                    g_window_state_handler(maximized);
                }
            }
            break;
        case WM_DPICHANGED:
            if (lparam) {
                const auto* suggested = reinterpret_cast<const RECT*>(lparam);
                ::SetWindowPos(hwnd,
                               nullptr,
                               suggested->left,
                               suggested->top,
                               suggested->right - suggested->left,
                               suggested->bottom - suggested->top,
                               SWP_NOZORDER | SWP_NOACTIVATE);
            }
            resize_webview_widget(hwnd);
            return 0;
        case WM_CLOSE:
            // close handler 返回 true 表示已消化(隐藏到托盘),不要 DestroyWindow。
            // 派发逻辑提取到 web_host_close_policy.hpp 的纯函数,unit test 共用。
            if (dispatch_wm_close(g_close_handler) == CloseDispatch::ConsumedByHandler) {
                return 0;
            }
            ::DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            ::PostQuitMessage(0);
            return 0;
        default:
            if (msg == kRequestQuitMsg) {
                // 来自 WebHost::request_quit 的真正退出信号 — 绕过 close handler。
                ::DestroyWindow(hwnd);
                return 0;
            }
            if (UINT focus_msg = focus_existing_instance_msg();
                focus_msg != 0 && msg == focus_msg) {
                // 第二个 acecode-desktop 进程检测到单例锁被占,通过 PostMessageW
                // 让我们把窗口拉前。等价于左键单击托盘 + close-to-tray 还原。
                if (::IsIconic(hwnd)) {
                    ::ShowWindow(hwnd, SW_RESTORE);
                } else {
                    ::ShowWindow(hwnd, SW_SHOW);
                }
                ::SetForegroundWindow(hwnd);
                return 0;
            }
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
        kDefaultDesktopWindowWidth,
        kDefaultDesktopWindowHeight,
        nullptr,
        nullptr,
        instance,
        nullptr);
    if (hwnd) {
        refresh_non_client_frame(hwnd);
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

namespace {

#if !defined(_WIN32) && !defined(__APPLE__)
struct GtkWindowApi {
    using GtkWidgetShow = void (*)(void*);
    using GtkWidgetHide = void (*)(void*);
    using GtkWindowPresent = void (*)(void*);
    using GtkWindowClose = void (*)(void*);
    using GSignalConnectData = unsigned long (*)(void*, const char*, void*, void*, void*, int);

    void* gtk = nullptr;
    void* gobject = nullptr;
    GtkWidgetShow widget_show = nullptr;
    GtkWidgetHide widget_hide = nullptr;
    GtkWindowPresent window_present = nullptr;
    GtkWindowClose window_close = nullptr;
    GSignalConnectData signal_connect_data = nullptr;

    bool load() {
        if (gtk && gobject) return true;
        gtk = ::dlopen("libgtk-3.so.0", RTLD_LAZY | RTLD_LOCAL);
        gobject = ::dlopen("libgobject-2.0.so.0", RTLD_LAZY | RTLD_LOCAL);
        if (!gtk || !gobject) {
            LOG_WARN("[desktop] GTK3/GObject runtime not available for window controls");
            return false;
        }
        auto sym = [](void* lib, const char* name) -> void* {
            return ::dlsym(lib, name);
        };
        widget_show = reinterpret_cast<GtkWidgetShow>(sym(gtk, "gtk_widget_show"));
        widget_hide = reinterpret_cast<GtkWidgetHide>(sym(gtk, "gtk_widget_hide"));
        window_present = reinterpret_cast<GtkWindowPresent>(sym(gtk, "gtk_window_present"));
        window_close = reinterpret_cast<GtkWindowClose>(sym(gtk, "gtk_window_close"));
        signal_connect_data = reinterpret_cast<GSignalConnectData>(
            sym(gobject, "g_signal_connect_data"));
        return widget_show && widget_hide && window_present && window_close &&
               signal_connect_data;
    }
};

GtkWindowApi& gtk_window_api() {
    static GtkWindowApi api;
    return api;
}

bool g_linux_force_close = false;

extern "C" int linux_window_delete_event(void*, void*, void*) {
    if (g_linux_force_close) return 0;
    return dispatch_wm_close(g_close_handler) == CloseDispatch::ConsumedByHandler ? 1 : 0;
}

void install_linux_close_handler(webview::webview& w) {
    auto window = w.window();
    if (!window.ok() || !window.value()) return;
    auto& api = gtk_window_api();
    if (!api.load()) return;
    api.signal_connect_data(window.value(),
                            "delete-event",
                            reinterpret_cast<void*>(linux_window_delete_event),
                            nullptr,
                            nullptr,
                            0);
}
#endif

std::pair<int, int> adjusted_window_size(int width, int height) {
#ifdef __APPLE__
    CGRect bounds = CGDisplayBounds(CGMainDisplayID());
    const double display_width = bounds.size.width;
    const double display_height = bounds.size.height;
    if (display_width > 0 && display_height > 0) {
        auto clamp_dimension = [](int value, double display, double margin, int preferred_min) {
            const int max_value = static_cast<int>(std::max(1.0, display - margin));
            const int min_value = std::min(preferred_min, max_value);
            return std::max(min_value, std::min(value, max_value));
        };
        width = clamp_dimension(width, display_width, 80.0, 900);
        height = clamp_dimension(height, display_height, 120.0, 640);
    }
#endif
    return {width, height};
}

} // namespace

struct WebHost::Impl {
    explicit Impl(bool debug, StartupWindowMode startup_mode)
#ifdef _WIN32
        : custom_window(startup_mode == StartupWindowMode::OffscreenUntilReady
                            ? create_offscreen_host_window(target_monitor)
                            : nullptr),
          offscreen_until_ready(custom_window != nullptr),
          com(custom_window != nullptr) {
        // 三段式构造:
        //   (1) 默认 Loader 路径,优先 offscreen custom_window;失败 → 切
        //       自管 nullptr 父窗口再试一次(沿用现有降级)。
        //   (2) (1) 整段还是抛 → 探测 Edge 浏览器自带的 msedgewebview2.exe
        //       目录,通过 WEBVIEW2_BROWSER_EXECUTABLE_FOLDER 环境变量
        //       (WebView2Loader.dll 公开的覆盖钩子)指过去再试。
        //   (3) Edge fallback 仍失败或没找到 Edge → 抛 WebHostInitializationError
        //       给 desktop main。main 还有最后一层 Edge --app=<daemon URL>
        //       兜底,不能在 WebHost 构造函数里直接 ExitProcess。
        auto make_webview_default_path = [&]() -> std::unique_ptr<webview::webview> {
            try {
                return std::make_unique<webview::webview>(
                    debug,
                    custom_window ? static_cast<void*>(custom_window) : nullptr);
            } catch (const webview::exception& e) {
                if (!custom_window) throw;
                LOG_WARN(std::string("[desktop] offscreen WebView host failed; "
                                     "falling back to default window: ") + e.what());
                if (::IsWindow(custom_window)) {
                    ::DestroyWindow(custom_window);
                }
                custom_window = nullptr;
                offscreen_until_ready = false;
                return std::make_unique<webview::webview>(debug, nullptr);
            }
        };

        try {
            w = make_webview_default_path();
        } catch (const webview::exception& e1) {
            LOG_WARN(std::string("[desktop] WebView2 default loader path failed: ") +
                     e1.what());
            auto edge_folder = find_edge_browser_folder();
            if (!edge_folder.has_value()) {
                LOG_ERROR("[desktop] no Microsoft Edge browser folder found to "
                          "fall back to embedded WebView2");
                throw WebHostInitializationError(
                    std::string("WebView2 default loader path failed and no "
                                "Edge WebView2 browser folder was found: ") +
                    e1.what());
            }
            const std::wstring folder_w = edge_folder->wstring();
            LOG_INFO(std::string("[desktop] retrying WebView2 with Edge browser "
                                 "folder: ") + edge_folder->string());
            if (!::SetEnvironmentVariableW(L"WEBVIEW2_BROWSER_EXECUTABLE_FOLDER",
                                           folder_w.c_str())) {
                LOG_ERROR("[desktop] SetEnvironmentVariableW("
                          "WEBVIEW2_BROWSER_EXECUTABLE_FOLDER) failed, last_error=" +
                          std::to_string(::GetLastError()));
            }
            try {
                // custom_window 在 default 路径内已被清掉(如果走过 offscreen),
                // 这里直接喂 nullptr 让 webview 自己造窗口最稳妥。
                w = std::make_unique<webview::webview>(debug, nullptr);
            } catch (const webview::exception& e2) {
                LOG_ERROR(std::string("[desktop] WebView2 Edge browser folder "
                                      "fallback also failed: ") + e2.what());
                throw WebHostInitializationError(
                    std::string("WebView2 Edge browser folder fallback failed: ") +
                    e2.what());
            }
        }
        if (custom_window) {
            resize_webview_widget(custom_window);
        }
        if (w) {
            configure_browser_defaults(*w);
        }
    }
#else
    {
        (void)startup_mode;
        w = std::make_unique<webview::webview>(debug, nullptr);
#if !defined(__APPLE__)
        install_linux_close_handler(*w);
#endif
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

    HWND hwnd() const {
        if (custom_window) return custom_window;
        if (!w) return nullptr;
        auto r = w->window();
        return r.ok() ? static_cast<HWND>(r.value()) : nullptr;
    }
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
    auto adjusted = adjusted_window_size(width, height);
    impl_->w->set_size(adjusted.first, adjusted.second, WEBVIEW_HINT_NONE);
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
    HWND hwnd = impl_->hwnd();
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
#if !defined(__APPLE__)
    auto window = impl_->w->window();
    if (!window.ok() || !window.value()) return;
    auto& api = gtk_window_api();
    if (!api.load()) return;
    if (visible) {
        api.widget_show(window.value());
        api.window_present(window.value());
    } else {
        api.widget_hide(window.value());
    }
#else
    (void)visible;
#endif
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
bool WebHost::start_window_drag() {
#ifdef _WIN32
    HWND hwnd = impl_->hwnd();
    if (!hwnd || !::IsWindow(hwnd)) return false;
    ::ReleaseCapture();
    ::SendMessageW(hwnd, WM_NCLBUTTONDOWN, HTCAPTION, 0);
    return true;
#else
    return false;
#endif
}
bool WebHost::start_window_resize(const std::string& direction) {
#ifdef _WIN32
    HWND hwnd = impl_->hwnd();
    if (!hwnd || !::IsWindow(hwnd)) return false;
    // 最大化窗口走 native resize 会被 Windows 解读为"拖出还原",体验诡异;
    // 直接拒绝,前端 strip 在最大化时也不应渲染。
    if (::IsZoomed(hwnd)) return false;
    auto area = parse_resize_direction(direction);
    if (!area) return false;
    const int ht = win32_hit_test_value(*area);
    // Caption / Client 不是 resize 命中,parse_resize_direction 已经过滤掉,
    // 这里二次保险:任何不是 resize 边/角的值都不发消息。
    switch (ht) {
        case HTLEFT: case HTRIGHT: case HTTOP: case HTBOTTOM:
        case HTTOPLEFT: case HTTOPRIGHT:
        case HTBOTTOMLEFT: case HTBOTTOMRIGHT:
            break;
        default:
            return false;
    }
    ::ReleaseCapture();
    ::SendMessageW(hwnd, WM_NCLBUTTONDOWN, ht, 0);
    return true;
#else
    (void)direction;
    return false;
#endif
}
bool WebHost::minimize_window() {
#ifdef _WIN32
    HWND hwnd = impl_->hwnd();
    if (!hwnd || !::IsWindow(hwnd)) return false;
    ::ShowWindow(hwnd, SW_MINIMIZE);
    return true;
#else
    return false;
#endif
}
bool WebHost::toggle_maximize_window() {
#ifdef _WIN32
    HWND hwnd = impl_->hwnd();
    if (!hwnd || !::IsWindow(hwnd)) return false;
    ::ShowWindow(hwnd, ::IsZoomed(hwnd) ? SW_RESTORE : SW_MAXIMIZE);
    return true;
#else
    return false;
#endif
}
bool WebHost::is_window_maximized() const {
#ifdef _WIN32
    HWND hwnd = impl_->hwnd();
    if (!hwnd || !::IsWindow(hwnd)) return false;
    return ::IsZoomed(hwnd) != FALSE;
#else
    return false;
#endif
}
void WebHost::set_window_state_change_handler(WindowStateHandler handler) {
#ifdef _WIN32
    g_window_state_handler = std::move(handler);
#else
    (void)handler;
#endif
}
bool WebHost::close_window() {
#ifdef _WIN32
    HWND hwnd = impl_->hwnd();
    if (!hwnd || !::IsWindow(hwnd)) return false;
    return ::PostMessageW(hwnd, WM_CLOSE, 0, 0) != FALSE;
#else
#if !defined(__APPLE__)
    if (dispatch_wm_close(g_close_handler) == CloseDispatch::ConsumedByHandler) {
        return true;
    }
    auto window = impl_->w->window();
    if (!window.ok() || !window.value()) return false;
    auto& api = gtk_window_api();
    if (!api.load()) return false;
    api.window_close(window.value());
    return true;
#else
    return false;
#endif
#endif
}
void WebHost::set_close_request_handler(std::function<bool()> handler) {
    g_close_handler = std::move(handler);
}
void WebHost::request_quit() {
#ifdef _WIN32
    HWND hwnd = impl_->hwnd();
    if (!hwnd || !::IsWindow(hwnd)) return;
    ::PostMessageW(hwnd, kRequestQuitMsg, 0, 0);
#else
#if !defined(__APPLE__)
    auto window = impl_->w->window();
    g_linux_force_close = true;
    if (window.ok() && window.value()) {
        auto& api = gtk_window_api();
        if (api.load()) {
            api.window_close(window.value());
        }
    }
#endif
    impl_->w->terminate();
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
