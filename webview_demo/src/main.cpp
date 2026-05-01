#include "webview.h"

#ifdef _WIN32
#include <windows.h>
#include <dwmapi.h>
#include <commctrl.h>
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "comctl32.lib")
#endif

#ifdef _WIN32
namespace {

LRESULT CALLBACK FramelessProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp,
                               UINT_PTR /*id*/, DWORD_PTR /*ref*/) {
    switch (msg) {
    case WM_NCCALCSIZE: {
        if (wp == TRUE) {
            // 关键:左/右/下保留默认 frame,让系统继续做 resize 命中
            // 同时让 DWM 把这层窗口当成正常 composed window 画阴影。
            // 只把 top frame 吃掉,这样视觉标题栏可以画到客户区最顶。
            int padding  = GetSystemMetrics(SM_CXPADDEDBORDER);
            int borderLR = GetSystemMetrics(SM_CXFRAME) + padding;
            int borderTB = GetSystemMetrics(SM_CYFRAME) + padding;
            auto *p = reinterpret_cast<NCCALCSIZE_PARAMS *>(lp);
            p->rgrc[0].left   += borderLR;
            p->rgrc[0].right  -= borderLR;
            p->rgrc[0].bottom -= borderTB;
            // top 不动 → 客户区从窗口顶起,标题栏可以画到最顶
            if (IsZoomed(hwnd)) {
                // maximized 时整窗已超出工作区一圈 frame,补回 top 防越界
                p->rgrc[0].top += borderTB;
            }
            return 0;
        }
        break;
    }
    case WM_NCHITTEST: {
        // 借用系统默认命中:在 WS_THICKFRAME 窗口的左/右/下 8px frame 上
        // 系统会自动给 HTLEFT/HTTOP/HTBOTTOMRIGHT/...,光标也会自动变。
        // 客户区里我们一律返回 HTCLIENT 让 WebView2 拿事件,
        // 标题栏拖拽走 mousedown -> SendMessage(WM_NCLBUTTONDOWN, HTCAPTION)。
        LRESULT hit = DefSubclassProc(hwnd, msg, wp, lp);
        if (hit >= HTLEFT && hit <= HTBOTTOMRIGHT) return hit;
        return HTCLIENT;
    }
    case WM_NCACTIVATE:
        // 抑制激活/失活时系统在我们盖掉的 NC 顶部区域画白线
        return DefWindowProc(hwnd, WM_NCACTIVATE, wp, -1);
    }
    return DefSubclassProc(hwnd, msg, wp, lp);
}
} // namespace
#endif

#ifdef _WIN32
int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int) {
#else
int main() {
#endif
    webview::webview w(false, nullptr);
    w.set_title("Webview Demo");
    w.set_size(900, 600, WEBVIEW_HINT_NONE);

#ifdef _WIN32
    HWND hwnd = static_cast<HWND>(w.window().value());

    // 关键:DWM 接管阴影、Win11 圆角、最小化/还原动画
    // {0,0,1,0} = 顶部留 1 device unit 的 glass 帧,这是触发 DWM 阴影
    // 同时让 NCCALCSIZE 把客户区铺满的最小骇客
    MARGINS margins{ 0, 0, 1, 0 };
    DwmExtendFrameIntoClientArea(hwnd, &margins);

    SetWindowSubclass(hwnd, FramelessProc, 1, 0);

    // 触发 NCCALCSIZE 重算,样式不变但客户区即刻铺满
    SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
                 SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE |
                 SWP_NOZORDER | SWP_NOOWNERZORDER);

    w.bind("drag_window", [hwnd](std::string) -> std::string {
        ReleaseCapture();
        SendMessage(hwnd, WM_NCLBUTTONDOWN, HTCAPTION, 0);
        return "";
    });
    w.bind("minimize_window", [hwnd](std::string) -> std::string {
        ShowWindow(hwnd, SW_MINIMIZE);
        return "";
    });
    w.bind("toggle_maximize", [hwnd](std::string) -> std::string {
        ShowWindow(hwnd, IsZoomed(hwnd) ? SW_RESTORE : SW_MAXIMIZE);
        return "";
    });
#else
    w.bind("drag_window",     [](std::string) -> std::string { return ""; });
    w.bind("minimize_window", [](std::string) -> std::string { return ""; });
    w.bind("toggle_maximize", [](std::string) -> std::string { return ""; });
#endif

    w.bind("close_app", [&](std::string) -> std::string {
        w.terminate();
        return "";
    });

    w.set_html(R"html(
        <!DOCTYPE html>
        <html>
        <head>
            <meta charset="UTF-8">
            <title>Webview Demo</title>
            <style>
                html, body {
                    margin: 0;
                    height: 100%;
                    font-family: "Segoe UI Variable", "Segoe UI", Arial, sans-serif;
                    background: #f3f3f3;
                    overflow: hidden;
                }
                .titlebar {
                    height: 32px;
                    background: rgba(243, 243, 243, 0.85);
                    backdrop-filter: blur(20px);
                    display: flex;
                    align-items: stretch;
                    user-select: none;
                    -webkit-user-select: none;
                    border-bottom: 1px solid rgba(0,0,0,0.06);
                }
                .titlebar-drag {
                    flex: 1;
                    display: flex;
                    align-items: center;
                    padding-left: 14px;
                    font-size: 12px;
                    color: #1a1a1a;
                    -webkit-app-region: drag; /* Edge ignores, kept for clarity */
                }
                .titlebar-btn {
                    width: 46px;
                    border: 0;
                    background: transparent;
                    color: #1a1a1a;
                    font-size: 11px;
                    cursor: default;
                    transition: background-color 80ms ease;
                }
                .titlebar-btn:hover { background: rgba(0,0,0,0.06); }
                .titlebar-btn.close:hover { background: #c42b1c; color: #fff; }

                .content {
                    height: calc(100% - 32px);
                    overflow: auto;
                    display: flex;
                    align-items: center;
                    justify-content: center;
                    padding: 24px;
                    box-sizing: border-box;
                }
                .card {
                    text-align: center;
                    background: #fff;
                    padding: 28px 40px;
                    border-radius: 8px;
                    box-shadow: 0 1px 2px rgba(0,0,0,0.06),
                                0 8px 24px rgba(0,0,0,0.08);
                    max-width: 520px;
                }
                h1 { color: #1a1a1a; margin: 0 0 8px; font-weight: 600; font-size: 22px; }
                p  { color: #555;    margin: 0 0 16px; line-height: 1.5; }
                button.primary {
                    background: #0067c0;
                    color: #fff;
                    border: 0;
                    padding: 8px 20px;
                    border-radius: 4px;
                    font-size: 13px;
                    font-weight: 500;
                    cursor: pointer;
                }
                button.primary:hover { background: #005ba1; }

                @media (prefers-color-scheme: dark) {
                    body { background: #202020; }
                    .titlebar { background: rgba(32,32,32,0.85); border-bottom-color: rgba(255,255,255,0.06); }
                    .titlebar-drag, .titlebar-btn { color: #f0f0f0; }
                    .titlebar-btn:hover { background: rgba(255,255,255,0.08); }
                    .card { background: #2b2b2b; box-shadow: 0 1px 2px rgba(0,0,0,0.4), 0 8px 24px rgba(0,0,0,0.4); }
                    h1 { color: #f5f5f5; }
                    p  { color: #c0c0c0; }
                }
            </style>
        </head>
        <body>
            <div class="titlebar">
                <div class="titlebar-drag" id="drag">Webview Demo</div>
                <button class="titlebar-btn"       onclick="window.minimize_window()" title="Minimize">&#xE921;</button>
                <button class="titlebar-btn"       onclick="window.toggle_maximize()" title="Maximize" id="maxbtn">&#xE922;</button>
                <button class="titlebar-btn close" onclick="window.close_app()"       title="Close">&#xE8BB;</button>
            </div>
            <div class="content">
                <div class="card">
                    <h1>Native shadow, native resize</h1>
                    <p>Drag the bar &mdash; Aero Snap works.<br>
                       Drag the window edges &mdash; system resize.<br>
                       Double-click the bar to maximize.</p>
                    <button class="primary" onclick="window.close_app()">Close App</button>
                </div>
            </div>
            <script>
                /* Use Segoe Fluent Icons for buttons (built into Win11) */
                document.querySelectorAll('.titlebar-btn').forEach(b => {
                    b.style.fontFamily = '"Segoe Fluent Icons", "Segoe MDL2 Assets"';
                    b.style.fontSize = '10px';
                });

                const drag = document.getElementById('drag');
                drag.addEventListener('mousedown', (e) => {
                    if (e.button === 0) window.drag_window();
                });
                drag.addEventListener('dblclick', () => window.toggle_maximize());
            </script>
        </body>
        </html>
    )html");

    w.run();
    return 0;
}
