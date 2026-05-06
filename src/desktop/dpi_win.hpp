#pragma once

// Windows 桌面壳 DPI 感知初始化。
//
// 必须在任何 HWND 创建之前调一次:webview/webview 只对自己创建的窗口设
// DPI awareness,而我们使用自定义的 Win32 父窗口承载 WebView2,所以得自己
// 调用 SetProcessDpiAwarenessContext / SetProcessDpiAwareness / SetProcessDPIAware
// 之中能用的那个。
//
// 三层回退顺序(失败则尝试下一层):
//   1. user32!SetProcessDpiAwarenessContext (Win10 1703+,PerMonitorAwareV2)
//   2. shcore!SetProcessDpiAwareness (Win8.1+,PerMonitorAware)
//   3. user32!SetProcessDPIAware (Vista+,SystemAware)
//
// ERROR_ACCESS_DENIED 视为成功 — 表示 manifest 里已经声明过,系统不接受重复设置。
//
// 仅在 Windows 编译时定义。

namespace acecode::desktop {

// 返回 true 表示成功设置(包括 ERROR_ACCESS_DENIED 的"已设置"情况),
// false 表示三层回退全部失败。失败不致命,主流程继续 — DPI 渲染将以
// 默认的 system-aware 行为运行。
bool enable_desktop_dpi_awareness();

} // namespace acecode::desktop
