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
#include <string>

namespace acecode::desktop {

class WebHost {
public:
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
