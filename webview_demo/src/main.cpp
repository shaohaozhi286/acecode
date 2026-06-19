#include "webview.h"

#ifdef _WIN32
#include <windows.h>
#include <windowsx.h>
#include <dwmapi.h>
#include <objbase.h>
#include <unknwn.h>
#include <WebView2.h>

#include <cstdint>
#include <sstream>
#include <string>

#pragma comment(lib, "dwmapi.lib")

namespace {

constexpr int kUnderlayWidth = 980;
constexpr int kUnderlayHeight = 620;
constexpr int kProbeWidth = 820;
constexpr int kProbeHeight = 500;
constexpr COLORREF kTransparentKey = RGB(255, 0, 255);
constexpr wchar_t kProbeStateProperty[] = L"WebViewAcrylicProbeState";

enum class BackdropMode {
    Clear,
    AccentAcrylic,
    AccentAcrylicDark,
    AccentBlur,
    SystemMica,
    SystemAcrylic,
    SystemTabbedMica,
};

struct ProbeWindowState {
    WNDPROC previous_proc = nullptr;
    BackdropMode mode = BackdropMode::Clear;
};

using SetWindowCompositionAttributeFn =
    BOOL(WINAPI *)(HWND, struct WindowCompositionAttributeData *);

enum WindowCompositionAttribute {
    WCA_ACCENT_POLICY = 19,
};

enum AccentState {
    ACCENT_DISABLED = 0,
    ACCENT_ENABLE_GRADIENT = 1,
    ACCENT_ENABLE_TRANSPARENTGRADIENT = 2,
    ACCENT_ENABLE_BLURBEHIND = 3,
    ACCENT_ENABLE_ACRYLICBLURBEHIND = 4,
    ACCENT_ENABLE_HOSTBACKDROP = 5,
};

struct AccentPolicy {
    int accent_state;
    int accent_flags;
    int gradient_color;
    int animation_id;
};

struct WindowCompositionAttributeData {
    WindowCompositionAttribute attribute;
    void *data;
    size_t size_of_data;
};

enum DwmWindowAttribute {
    DWMWA_SYSTEMBACKDROP_TYPE_COMPAT = 38,
};

enum DwmSystemBackdropType {
    DWMSBT_AUTO_COMPAT = 0,
    DWMSBT_NONE_COMPAT = 1,
    DWMSBT_MAINWINDOW_COMPAT = 2,
    DWMSBT_TRANSIENTWINDOW_COMPAT = 3,
    DWMSBT_TABBEDWINDOW_COMPAT = 4,
};

std::string hresult_hex(HRESULT hr) {
    std::ostringstream stream;
    stream << "0x" << std::hex << std::uppercase
           << static_cast<std::uint32_t>(hr);
    return stream.str();
}

void fill_transparent_key(HWND hwnd, HDC hdc) {
    RECT rect{};
    GetClientRect(hwnd, &rect);
    HBRUSH brush = CreateSolidBrush(kTransparentKey);
    FillRect(hdc, &rect, brush);
    DeleteObject(brush);
}

void paint_underlay(HWND hwnd, HDC hdc) {
    RECT rect{};
    GetClientRect(hwnd, &rect);

    HBRUSH bg = CreateSolidBrush(RGB(245, 247, 250));
    FillRect(hdc, &rect, bg);
    DeleteObject(bg);

    const COLORREF colors[] = {
        RGB(28, 118, 210), RGB(236, 72, 153), RGB(22, 163, 74),
        RGB(245, 158, 11), RGB(99, 102, 241), RGB(14, 165, 233),
    };

    for (int x = -rect.bottom; x < rect.right + rect.bottom; x += 58) {
        POINT points[4] = {{x, 0},
                           {x + 28, 0},
                           {x + rect.bottom + 28, rect.bottom},
                           {x + rect.bottom, rect.bottom}};
        HBRUSH brush = CreateSolidBrush(colors[(x / 58 + 20) % 6]);
        HBRUSH old = static_cast<HBRUSH>(SelectObject(hdc, brush));
        HPEN pen = CreatePen(PS_SOLID, 1, colors[(x / 58 + 20) % 6]);
        HPEN old_pen = static_cast<HPEN>(SelectObject(hdc, pen));
        Polygon(hdc, points, 4);
        SelectObject(hdc, old_pen);
        SelectObject(hdc, old);
        DeleteObject(pen);
        DeleteObject(brush);
    }

    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, RGB(17, 24, 39));
    HFONT font = CreateFontW(30, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                             DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                             CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                             DEFAULT_PITCH, L"Segoe UI");
    HFONT old_font = static_cast<HFONT>(SelectObject(hdc, font));
    RECT text_rect{32, 28, rect.right - 32, 120};
    DrawTextW(hdc, L"UNDERLAY WINDOW - this must be visible through transparent WebView areas",
              -1, &text_rect, DT_LEFT | DT_TOP | DT_WORDBREAK);
    SelectObject(hdc, old_font);
    DeleteObject(font);
}

LRESULT CALLBACK UnderlayProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps{};
        HDC hdc = BeginPaint(hwnd, &ps);
        paint_underlay(hwnd, hdc);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    default:
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
}

HWND create_underlay_window(HINSTANCE instance) {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.hInstance = instance;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpfnWndProc = UnderlayProc;
    wc.lpszClassName = L"WebViewAcrylicProbeUnderlay";
    RegisterClassExW(&wc);

    HWND hwnd = CreateWindowExW(0, wc.lpszClassName,
                                L"WebView2 transparency underlay",
                                WS_OVERLAPPEDWINDOW | WS_VISIBLE, 96, 96,
                                kUnderlayWidth, kUnderlayHeight, nullptr,
                                nullptr, instance, nullptr);
    return hwnd;
}

void disable_system_backdrop(HWND hwnd) {
    const int backdrop = DWMSBT_NONE_COMPAT;
    DwmSetWindowAttribute(hwnd, DWMWA_SYSTEMBACKDROP_TYPE_COMPAT, &backdrop,
                          sizeof(backdrop));
}

void set_system_backdrop(HWND hwnd, int backdrop) {
    DwmSetWindowAttribute(hwnd, DWMWA_SYSTEMBACKDROP_TYPE_COMPAT, &backdrop,
                          sizeof(backdrop));
}

void set_accent_policy(HWND hwnd, AccentState state, DWORD gradient_color) {
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    auto fn = reinterpret_cast<SetWindowCompositionAttributeFn>(
        GetProcAddress(user32, "SetWindowCompositionAttribute"));
    if (!fn) {
        return;
    }

    AccentPolicy policy{};
    policy.accent_state = state;
    policy.gradient_color = static_cast<int>(gradient_color);
    policy.accent_flags = 2;

    WindowCompositionAttributeData data{};
    data.attribute = WCA_ACCENT_POLICY;
    data.data = &policy;
    data.size_of_data = sizeof(policy);
    fn(hwnd, &data);
}

void set_layered_color_key(HWND hwnd, bool enabled) {
    LONG_PTR ex_style = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    if (enabled) {
        ex_style |= WS_EX_LAYERED;
    } else {
        ex_style &= ~WS_EX_LAYERED;
    }
    SetWindowLongPtrW(hwnd, GWL_EXSTYLE, ex_style);
    if (enabled) {
        SetLayeredWindowAttributes(hwnd, kTransparentKey, 255, LWA_COLORKEY);
    }
}

void apply_backdrop_mode(HWND hwnd, BackdropMode mode) {
    auto *state = reinterpret_cast<ProbeWindowState *>(
        GetPropW(hwnd, kProbeStateProperty));
    if (state) {
        state->mode = mode;
    }

    switch (mode) {
    case BackdropMode::Clear:
        set_accent_policy(hwnd, ACCENT_DISABLED, 0);
        disable_system_backdrop(hwnd);
        set_layered_color_key(hwnd, true);
        break;
    case BackdropMode::AccentAcrylic:
        disable_system_backdrop(hwnd);
        set_layered_color_key(hwnd, false);
        set_accent_policy(hwnd, ACCENT_ENABLE_ACRYLICBLURBEHIND, 0x88DDDDDD);
        break;
    case BackdropMode::AccentAcrylicDark:
        disable_system_backdrop(hwnd);
        set_layered_color_key(hwnd, false);
        set_accent_policy(hwnd, ACCENT_ENABLE_ACRYLICBLURBEHIND, 0x88222222);
        break;
    case BackdropMode::AccentBlur:
        disable_system_backdrop(hwnd);
        set_layered_color_key(hwnd, false);
        set_accent_policy(hwnd, ACCENT_ENABLE_BLURBEHIND, 0x88DDDDDD);
        break;
    case BackdropMode::SystemMica:
        set_accent_policy(hwnd, ACCENT_DISABLED, 0);
        set_layered_color_key(hwnd, false);
        set_system_backdrop(hwnd, DWMSBT_MAINWINDOW_COMPAT);
        break;
    case BackdropMode::SystemAcrylic:
        set_accent_policy(hwnd, ACCENT_DISABLED, 0);
        set_layered_color_key(hwnd, false);
        set_system_backdrop(hwnd, DWMSBT_TRANSIENTWINDOW_COMPAT);
        break;
    case BackdropMode::SystemTabbedMica:
        set_accent_policy(hwnd, ACCENT_DISABLED, 0);
        set_layered_color_key(hwnd, false);
        set_system_backdrop(hwnd, DWMSBT_TABBEDWINDOW_COMPAT);
        break;
    }
    RedrawWindow(hwnd, nullptr, nullptr,
                 RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
}

void apply_irregular_region(HWND hwnd) {
    HRGN base = CreateRoundRectRgn(0, 0, kProbeWidth + 1, kProbeHeight + 1, 38,
                                   38);
    HRGN notch = CreateEllipticRgn(-92, kProbeHeight - 184, 130,
                                   kProbeHeight + 38);
    CombineRgn(base, base, notch, RGN_DIFF);
    DeleteObject(notch);
    SetWindowRgn(hwnd, base, TRUE);
}

LRESULT CALLBACK ProbeProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto *state = reinterpret_cast<ProbeWindowState *>(
        GetPropW(hwnd, kProbeStateProperty));
    switch (msg) {
    case WM_ERASEBKGND:
        if (!state || state->mode == BackdropMode::Clear) {
            fill_transparent_key(hwnd, reinterpret_cast<HDC>(wp));
            return 1;
        }
        break;
    case WM_PAINT:
        if (!state || state->mode == BackdropMode::Clear) {
            PAINTSTRUCT ps{};
            HDC hdc = BeginPaint(hwnd, &ps);
            fill_transparent_key(hwnd, hdc);
            EndPaint(hwnd, &ps);
            return 0;
        }
        break;
    case WM_NCHITTEST: {
        POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        ScreenToClient(hwnd, &pt);
        if (pt.y < 38) {
            return HTCAPTION;
        }
        break;
    }
    case WM_NCDESTROY:
        RemovePropW(hwnd, kProbeStateProperty);
        break;
    }

    if (state && state->previous_proc) {
        return CallWindowProcW(state->previous_proc, hwnd, msg, wp, lp);
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void install_probe_window_proc(HWND hwnd, ProbeWindowState &state) {
    state.previous_proc = reinterpret_cast<WNDPROC>(
        SetWindowLongPtrW(hwnd, GWLP_WNDPROC,
                          reinterpret_cast<LONG_PTR>(ProbeProc)));
    SetPropW(hwnd, kProbeStateProperty, &state);
}

void make_probe_window(HWND hwnd) {
    LONG_PTR style = GetWindowLongPtrW(hwnd, GWL_STYLE);
    style &= ~(WS_CAPTION | WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX |
               WS_SYSMENU);
    style |= WS_POPUP;
    SetWindowLongPtrW(hwnd, GWL_STYLE, style);

    MARGINS margins{-1, -1, -1, -1};
    DwmExtendFrameIntoClientArea(hwnd, &margins);

    SetWindowPos(hwnd, HWND_TOPMOST, 176, 156, kProbeWidth, kProbeHeight,
                 SWP_FRAMECHANGED | SWP_SHOWWINDOW);
    apply_irregular_region(hwnd);
}

std::string set_webview_transparent_background(webview::webview &w) {
    auto controller_result = w.browser_controller();
    if (!controller_result.ok()) {
        return "browser_controller unavailable";
    }

    auto *controller =
        static_cast<ICoreWebView2Controller *>(controller_result.value());
    ICoreWebView2Controller2 *controller2 = nullptr;
    HRESULT hr = controller->QueryInterface(IID_PPV_ARGS(&controller2));
    if (FAILED(hr) || !controller2) {
        return "ICoreWebView2Controller2 QueryInterface failed: " +
               hresult_hex(hr);
    }

    COREWEBVIEW2_COLOR transparent{};
    transparent.A = 0;
    transparent.R = 255;
    transparent.G = 255;
    transparent.B = 255;
    hr = controller2->put_DefaultBackgroundColor(transparent);
    controller2->Release();
    if (FAILED(hr)) {
        return "put_DefaultBackgroundColor failed: " + hresult_hex(hr);
    }
    return "WebView2 DefaultBackgroundColor alpha=0";
}

BackdropMode initial_backdrop_mode() {
    const std::wstring command_line = GetCommandLineW();
    if (command_line.find(L"--accent") != std::wstring::npos) {
        return BackdropMode::AccentAcrylic;
    }
    if (command_line.find(L"--dark-accent") != std::wstring::npos) {
        return BackdropMode::AccentAcrylicDark;
    }
    if (command_line.find(L"--blur") != std::wstring::npos) {
        return BackdropMode::AccentBlur;
    }
    if (command_line.find(L"--mica-alt") != std::wstring::npos ||
        command_line.find(L"--tabbed") != std::wstring::npos) {
        return BackdropMode::SystemTabbedMica;
    }
    if (command_line.find(L"--mica") != std::wstring::npos) {
        return BackdropMode::SystemMica;
    }
    if (command_line.find(L"--system") != std::wstring::npos) {
        return BackdropMode::SystemAcrylic;
    }
    return BackdropMode::Clear;
}

std::string mode_label(BackdropMode mode) {
    switch (mode) {
    case BackdropMode::Clear:
        return "Clear transparent";
    case BackdropMode::AccentAcrylic:
        return "Accent acrylic light";
    case BackdropMode::AccentAcrylicDark:
        return "Accent acrylic dark";
    case BackdropMode::AccentBlur:
        return "Accent blur";
    case BackdropMode::SystemMica:
        return "System mica";
    case BackdropMode::SystemAcrylic:
        return "System acrylic";
    case BackdropMode::SystemTabbedMica:
        return "System mica alt";
    }
    return "Unknown";
}

std::string html(std::string native_status, BackdropMode initial_mode) {
    return R"html(
<!doctype html>
<html>
<head>
  <meta charset="utf-8">
  <title>WebView2 Transparency Probe</title>
  <style>
    :root, html, body {
      width: 100%;
      height: 100%;
      margin: 0;
      background: transparent !important;
      overflow: hidden;
      font-family: "Segoe UI Variable", "Segoe UI", Arial, sans-serif;
      color: #111827;
    }
    * { box-sizing: border-box; }
    .stage {
      width: 100%;
      height: 100%;
      display: grid;
      grid-template-columns: 330px 1fr;
      background: transparent;
    }
    .drag {
      position: fixed;
      inset: 0 0 auto 0;
      height: 38px;
      z-index: 10;
      display: flex;
      align-items: center;
      gap: 8px;
      padding: 0 14px;
      color: rgba(17, 24, 39, .76);
      font-size: 12px;
      user-select: none;
      background: rgba(255, 255, 255, .24);
      border-bottom: 1px solid rgba(255, 255, 255, .36);
    }
    .dot {
      width: 8px;
      height: 8px;
      border-radius: 999px;
      background: #22c55e;
      box-shadow: 0 0 0 4px rgba(34, 197, 94, .18);
    }
    .transparent-zone {
      padding: 58px 18px 18px;
      min-width: 0;
      background:
        linear-gradient(135deg, rgba(255,255,255,.32), rgba(255,255,255,.08));
      border-right: 1px solid rgba(255,255,255,.34);
    }
    .glass {
      height: 100%;
      border: 1px dashed rgba(17, 24, 39, .45);
      border-radius: 18px;
      background: rgba(255,255,255,.08);
      padding: 18px;
      display: flex;
      flex-direction: column;
      justify-content: space-between;
      backdrop-filter: blur(0px);
    }
    .glass h1 {
      margin: 0;
      font-size: 18px;
      line-height: 1.25;
      font-weight: 650;
    }
    .glass p {
      margin: 8px 0 0;
      font-size: 13px;
      line-height: 1.45;
      color: rgba(17, 24, 39, .72);
    }
    .panel-wrap {
      padding: 58px 24px 24px;
      display: flex;
      align-items: center;
      justify-content: center;
      background: transparent;
    }
    .panel {
      width: min(360px, 100%);
      border-radius: 28px;
      background: rgba(255,255,255,.94);
      box-shadow:
        0 20px 50px rgba(15, 23, 42, .20),
        inset 0 0 0 1px rgba(255,255,255,.78);
      padding: 24px;
    }
    .pet {
      width: 148px;
      height: 148px;
      margin: 0 auto 18px;
      border-radius: 44% 56% 52% 48% / 48% 42% 58% 52%;
      background:
        radial-gradient(circle at 34% 34%, #fff 0 8px, transparent 9px),
        radial-gradient(circle at 64% 34%, #fff 0 8px, transparent 9px),
        radial-gradient(circle at 34% 34%, #111827 0 4px, transparent 5px),
        radial-gradient(circle at 64% 34%, #111827 0 4px, transparent 5px),
        linear-gradient(135deg, #38bdf8, #a78bfa 52%, #f472b6);
      box-shadow: inset 0 -20px 34px rgba(30, 41, 59, .18);
    }
    .panel h2 {
      margin: 0 0 8px;
      font-size: 19px;
      line-height: 1.25;
      font-weight: 650;
      text-align: center;
    }
    .panel p {
      margin: 0 0 16px;
      color: #4b5563;
      font-size: 13px;
      line-height: 1.45;
      text-align: center;
    }
    .modes {
      display: grid;
      grid-template-columns: repeat(4, minmax(0, 1fr));
      gap: 8px;
    }
    button {
      border: 1px solid rgba(15, 23, 42, .12);
      background: #fff;
      color: #111827;
      border-radius: 10px;
      height: 34px;
      font: inherit;
      font-size: 11px;
    }
    button:hover { background: #f3f4f6; }
    .status {
      margin-top: 14px;
      font-family: Consolas, "Cascadia Mono", monospace;
      font-size: 11px;
      color: #6b7280;
      text-align: center;
      overflow-wrap: anywhere;
    }
  </style>
</head>
<body>
  <div class="drag" onmousedown="if(event.button===0) window.drag_window()">
    <span class="dot"></span>
    <span>WebView2 transparent probe - drag here</span>
  </div>
  <main class="stage">
    <section class="transparent-zone">
      <div class="glass">
        <div>
          <h1>Transparent WebView area</h1>
          <p>If this works, the colored UNDERLAY WINDOW behind this probe is visible here.</p>
          <p>If this is white, gray, or magenta, the WebView/host transparency chain is still broken.</p>
        </div>
        <p>Mode starts as )html" + mode_label(initial_mode) +
           R"html(. Try Acrylic buttons to test host backdrop behavior.</p>
      </div>
    </section>
    <section class="panel-wrap">
      <div class="panel">
        <div class="pet"></div>
        <h2>Opaque island</h2>
        <p>This area is intentionally opaque so WebView rendering is easy to distinguish from host transparency.</p>
        <div class="modes">
          <button onclick="window.set_mode('clear')">Clear</button>
          <button onclick="window.set_mode('accent')">Accent</button>
          <button onclick="window.set_mode('dark-accent')">Dark</button>
          <button onclick="window.set_mode('blur')">Blur</button>
          <button onclick="window.set_mode('mica')">Mica</button>
          <button onclick="window.set_mode('system')">Acrylic</button>
          <button onclick="window.set_mode('mica-alt')">Mica Alt</button>
        </div>
        <div class="status">)html" + native_status +
           R"html(</div>
      </div>
    </section>
  </main>
</body>
</html>
)html";
}

} // namespace

int WINAPI WinMain(HINSTANCE instance, HINSTANCE, LPSTR, int) {
    SetEnvironmentVariableW(L"WEBVIEW2_DEFAULT_BACKGROUND_COLOR",
                            L"00FFFFFF");
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    HWND underlay = create_underlay_window(instance);

    webview::webview w(false, nullptr);
    w.set_title("WebView2 Transparency Probe");
    w.set_size(kProbeWidth, kProbeHeight, WEBVIEW_HINT_NONE);

    HWND hwnd = static_cast<HWND>(w.window().value());
    ProbeWindowState state{};
    install_probe_window_proc(hwnd, state);
    make_probe_window(hwnd);
    const BackdropMode initial_mode = initial_backdrop_mode();
    apply_backdrop_mode(hwnd, initial_mode);

    const std::string transparent_status = set_webview_transparent_background(w);

    w.bind("drag_window", [hwnd](std::string) -> std::string {
        ReleaseCapture();
        SendMessageW(hwnd, WM_NCLBUTTONDOWN, HTCAPTION, 0);
        return "";
    });

    w.bind("set_mode", [hwnd](std::string req) -> std::string {
        if (req.find("accent") != std::string::npos) {
            if (req.find("dark-accent") != std::string::npos) {
                apply_backdrop_mode(hwnd, BackdropMode::AccentAcrylicDark);
            } else {
                apply_backdrop_mode(hwnd, BackdropMode::AccentAcrylic);
            }
        } else if (req.find("blur") != std::string::npos) {
            apply_backdrop_mode(hwnd, BackdropMode::AccentBlur);
        } else if (req.find("mica-alt") != std::string::npos) {
            apply_backdrop_mode(hwnd, BackdropMode::SystemTabbedMica);
        } else if (req.find("mica") != std::string::npos) {
            apply_backdrop_mode(hwnd, BackdropMode::SystemMica);
        } else if (req.find("system") != std::string::npos) {
            apply_backdrop_mode(hwnd, BackdropMode::SystemAcrylic);
        } else {
            apply_backdrop_mode(hwnd, BackdropMode::Clear);
        }
        return "";
    });

    w.bind("close_app", [&](std::string) -> std::string {
        w.terminate();
        return "";
    });

    w.set_html(html(transparent_status, initial_mode));
    w.run();

    if (underlay) {
        DestroyWindow(underlay);
    }
    CoUninitialize();
    return 0;
}

#else

int main() {
    webview::webview w(false, nullptr);
    w.set_title("Webview Demo");
    w.set_size(900, 600, WEBVIEW_HINT_NONE);
    w.set_html("<!doctype html><html><body><h1>Windows-only transparency "
               "probe</h1></body></html>");
    w.run();
    return 0;
}

#endif
