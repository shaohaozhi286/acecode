# macOS desktop 标题栏去除备忘

## 背景

ACECode desktop 使用 `webview/webview` 在 macOS 上承载 `WKWebView`。默认
`NSWindow` 会带 macOS 原生标题栏和红黄绿窗口按钮。前端已经实现了自己的
TopBar 和窗口控制按钮,因此 desktop shell 需要把原生标题栏区域让给前端。

这次问题表现为:红黄绿按钮已经隐藏,但窗口顶部仍然保留一块白色原生标题栏
高度,导致前端 TopBar 下移。

## 不要使用的方案

不要在 `WKWebView` 已经挂载到 `NSWindow` 后强行切到真正 borderless:

- `styleMask &= ~NSWindowStyleMaskTitled`
- `styleMask = NSWindowStyleMaskResizable`
- `setContentView:nil` 后再 reattach
- 运行时替换自定义 `NSWindow` 再迁移 WebView

在 macOS 12.7.4 / WebKit 17613 上,这些路径会触发
`WKWindowVisibilityObserver` 移除 KVO observer 时的 `EXC_BAD_ACCESS`。
崩溃栈会落在 `-[NSWindow setStyleMask:]` 或 `-[NSWindow setContentView:]`
附近。

也不要把一个 0x0 的自建 `NSWindow` 传给当前版本 `webview/webview` 再依赖
后续 `set_size()` 展示。该路径没有稳定生成可见主窗口,只会留下菜单栏/status
item。

## 当前安全方案

最终实现保留 `NSWindowStyleMaskTitled`,但启用 full-size content:

- `NSWindowStyleMaskFullSizeContentView`
- `setTitleVisibility:NSWindowTitleHidden`
- `setTitlebarAppearsTransparent:YES`
- 隐藏 close / minimize / zoom 标准按钮
- 找到 `contentView` 的 frame view,把 `contentView` frame 设成 frame view
  bounds,并隐藏 frame view 中除 content view 外的标题栏子视图

关键点是 macOS 下不要再调用 `webview/webview` 的 `set_size()`。该实现会重新
设置 styleMask 为 `Titled | Closable | Miniaturizable | Resizable`,从而覆盖
`FullSizeContentView`,白色标题栏会回来。

因此 `WebHost::set_size()` 在 macOS 上直接走 AppKit:

- `setFrame:NSMakeRect(...)`
- `center`
- `makeKeyAndOrderFront`
- 再调用 `configure_mac_window_chrome()`

这样既保留了 `WKWebView` 与 AppKit 兼容的 titled window 生命周期,又让前端
内容铺满原生标题栏区域。

## 验证方式

修改后需要重新编译 desktop 并实际截图验证:

```bash
cmake --build build/macos-x64-desktop-release --target acecode-desktop --config Release
open -n build/macos-x64-desktop-release/ACECode.app
screencapture -x /tmp/acecode-final-titlebar-verify.png
```

如果 `screencapture` 报 display asleep,先唤醒显示器:

```bash
caffeinate -u -t 5
```

验证标准:

- ACECode 窗口顶部直接显示前端 TopBar。
- 没有额外的 macOS 原生白色标题栏。
- 没有红黄绿原生窗口按钮。
- 应用不出现 macOS crash reporter。

