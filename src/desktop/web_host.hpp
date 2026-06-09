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

    // debug=true 时启用 WebView2 DevTools(F12 / 右键检查可用)。MVP 默认 true。
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

    // debug 模式下打开 WebView 开发者工具。非 WebView2 平台返回 false。
    bool open_dev_tools();

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
