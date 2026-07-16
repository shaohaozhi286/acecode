#pragma once

// 极薄 wrapper over webview/webview,集中库依赖,让 main.cpp 不直接 include
// webview.h(便于以后切换到原生 WebView2 SDK 或 wxWebView 时只动这一文件)。
//
// 多 workspace 模型新增 bind / eval / native_window:
//   - bind: 注册 JS 全局函数 window.aceDesktop_xxx,从前端调用返回 JSON 字符串
//   - eval: 主线程上注入 JS,用来推送"workspace 切换"等事件
//   - native_window: 取 HWND/NSWindow*/GtkWindow*,folder picker 当 parent 用
//
// 所有方法必须在主线程上调(WebHost::run 之前 bind 安全)。

#include <functional>
#include <stdexcept>
#include <string>
#include <vector>

namespace acecode::desktop {

inline constexpr int kDefaultDesktopWindowWidth = 1280;
inline constexpr int kDefaultDesktopWindowHeight = 820;
inline constexpr int kMinimumDesktopWindowWidth = 1170;

class WebHostInitializationError : public std::runtime_error {
public:
    explicit WebHostInitializationError(const std::string& message)
        : std::runtime_error(message) {}
};

class WebHost {
public:
    struct PointerEvent {
        int button = 1;
        int root_x = 0;
        int root_y = 0;
        unsigned int timestamp = 0;
        bool has_position = false;
    };

    enum class StartupWindowMode {
        DefaultVisible,
        OffscreenUntilReady,
    };

    // debug=true 时启用 WebView2 默认 DevTools 入口(F12 / 右键检查)。
    // ACECode 另行保留 F11/on-demand DevTools,用于 release desktop 诊断。
    explicit WebHost(bool debug = true,
                     StartupWindowMode startup_mode = StartupWindowMode::DefaultVisible);
    ~WebHost();

    WebHost(const WebHost&) = delete;
    WebHost& operator=(const WebHost&) = delete;

    void set_title(const std::string& title);
    void set_size(int width, int height);
    void navigate(const std::string& url);

    // 显示/隐藏 native 窗口。注意:启动路径不要用 hide-before-navigate。
    // WebView2 controller 在 hidden parent 下可能暂停渲染,导致页面一直空白。
    void set_visible(bool visible);

    // 注入一段 JS 在每次 navigate 前执行(`window.__ACECODE_INITIAL_*` 之类常量
    // 在这里塞)。在 navigate 之前调。
    void init_script(const std::string& js);

    // 当 webview run 中时,在主线程上 eval 一段 JS。线程安全(底层 webview_dispatch)。
    void eval(const std::string& js);

    // 打开 WebView 开发者工具。Windows 会按需启用 DevTools;非 WebView2
    // 平台目前返回 false。
    bool open_dev_tools();

    // 设置窗口打底色(快速 resize 时新暴露区域的擦除色):host/webview_widget
    // 两层窗口类背景刷 + WebView2 DefaultBackgroundColor 一起换。入参
    // "#RRGGBB" / "RRGGBB",非法形态或非 Windows 平台返回 false(哨兵,不猜)。
    // 前端 ThemeProvider 在主题变化时经 aceDesktop_setWindowBackgroundColor
    // 推送,启动默认为浅色 body 底色(window_background.hpp)。GUI 主线程 only
    // (bind 回调天然满足)。
    bool set_background_color(const std::string& color_text);

    // Frameless desktop chrome helpers. Windows uses native non-client messages,
    // Linux uses GTK move/resize/window-state APIs, and macOS uses Cocoa window
    // operations.
    bool start_window_drag(const PointerEvent& event);
    // 从 JS 端发起原生 resize:WebView2 子窗口默认会吃掉 WM_NCHITTEST,导致
    // 父窗口在子窗口覆盖区域拿不到 resize 命中。前端在窗口边缘 strip 上 mousedown
    // 时调这个方法,内部 ReleaseCapture + WM_NCLBUTTONDOWN(HT*) 让 Windows
    // 进入和"鼠标真在 NC 区按下"等价的原生 resize 循环 — Aero snap / Win+方向键
    // 这些系统手势也跟着可用。direction 见 parse_resize_direction。最大化时拒绝
    // 调用并返回 false,避免 Windows 从屏幕边拉出诡异的还原行为。
    bool start_window_resize(const std::string& direction,
                             const PointerEvent& event);
    bool minimize_window();
    bool toggle_maximize_window();
    // 当前窗口是否处于最大化状态(IsZoomed / gtk maximized / NSWindow zoomed)。
    // 前端 TopBar 在 mount 时调一次拿初始态,之后靠 set_window_state_change_handler
    // 推送的变更事件维护。
    bool is_window_maximized() const;

    struct WebCoreInfo {
        std::string backend;
        std::string name;
        std::string version;
        std::string detail;
        std::string runtime_path;
        std::string wrapper_name;
        std::string wrapper_version;
    };

    // 返回当前 desktop shell 使用的 Web 核心运行时信息。Windows 优先报告
    // 实际 WebView2/Edge runtime;Linux 报告 WebKitGTK;macOS 报告 WKWebView/WebKit。
    WebCoreInfo web_core_info() const;

    // 注册"窗口最大化/还原状态变化" handler。WM_SIZE 时检测 IsZoomed 与上次缓存
    // 是否不同,不同才触发 — SIZE_MINIMIZED → SIZE_RESTORED 之类的中间过渡不会
    // 重复抛事件。非 Windows 平台为 stub。
    using WindowStateHandler = std::function<void(bool maximized)>;
    void set_window_state_change_handler(WindowStateHandler handler);

    // Windows host activation is not guaranteed to surface as a DOM window.focus
    // event in WebView2. Register a native activation callback so the frontend can
    // explicitly restore the chat composer focus after the Desktop window returns.
    using WindowFocusHandler = std::function<void()>;
    void set_window_focus_handler(WindowFocusHandler handler);

    // 关闭请求(× / Alt+F4 / aceDesktop_closeWindow)。会先派发到
    // close_request_handler;handler 返回 true 表示已消化(典型如"隐藏到托盘"),
    // 返回 false / 未注册 → 走原 DestroyWindow 退出路径。
    bool close_window();

    // 注册"关窗请求"拦截 handler。回调返回 true → 不 DestroyWindow,留住进程。
    // 见 openspec/changes/enhance-desktop-tray-menu(close_to_tray)。
    void set_close_request_handler(std::function<bool()> handler);

    // 真正退出请求 — 绕过 close_request_handler 直接 DestroyWindow + PostQuitMessage。
    // 用于托盘 "退出" 菜单等需要无条件退出的入口。
    void request_quit();

    // 注册「系统文件拖放」handler。Windows 拦截 WebView2 的 file:// 导航、macOS
    // swizzle WKWebView 拖放后,把拖入文件的完整路径回传给这里(Windows 为
    // file:// URI、macOS 为本地路径,前端纯函数统一归一化);main.cpp 再经 eval
    // 注入到前端控制台。Linux/WebKitGTK 由前端直接读 text/uri-list,不调本 handler。
    // 必须在 run() 之前注册(内部据此安装平台拦截)。
    using FileDropHandler = std::function<void(std::vector<std::string> paths)>;
    void set_file_drop_handler(FileDropHandler handler);

    // 注册同步 binding。fn 接到的是 JSON array 字符串(JS 端调时传的实参打包),
    // 返回的字符串必须是合法 JSON value(对象/数组/字符串字面/数字/null)。
    using SyncHandler = std::function<std::string(const std::string& args_json)>;
    void bind(const std::string& name, SyncHandler fn);

    // 阻塞,直到窗口关闭。
    void run();

    // Windows: HWND;否则 nullptr。MVP 暴露给 folder picker。
    void* native_window() const;

private:
    struct Impl;
    Impl* impl_;
};

} // namespace acecode::desktop
