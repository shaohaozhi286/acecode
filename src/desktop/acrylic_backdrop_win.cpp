#include "acrylic_backdrop_win.hpp"

#ifdef _WIN32

#  include "../utils/logger.hpp"

#  include <unknwn.h>
#  include <WebView2.h>
#  include <d2d1_2.h>
#  include <d2d1_2helper.h>
#  include <d3d11_2.h>
#  include <dcomp.h>
#  include <dwmapi.h>
#  include <dxgi1_3.h>
#  include <wrl/client.h>

#  include <algorithm>
#  include <memory>
#  include <mutex>
#  include <sstream>

namespace acecode::desktop {
namespace {

using Microsoft::WRL::ComPtr;

#  ifndef DWMWA_SYSTEMBACKDROP_TYPE
constexpr DWORD kDwmWindowAttributeSystemBackdropType = 38;
#  else
constexpr DWORD kDwmWindowAttributeSystemBackdropType = DWMWA_SYSTEMBACKDROP_TYPE;
#  endif
#  ifndef DWMWA_WINDOW_CORNER_PREFERENCE
constexpr DWORD kDwmWindowAttributeWindowCornerPreference = 33;
#  else
constexpr DWORD kDwmWindowAttributeWindowCornerPreference = DWMWA_WINDOW_CORNER_PREFERENCE;
#  endif

constexpr int kDwmSystemBackdropTransientWindow = 3;
constexpr int kDwmSystemBackdropNone = 1;
constexpr int kDwmWindowCornerPreferenceRound = 2;
constexpr long kWindows11RoundCornersBuild = 22000;
constexpr long kWindows11SystemBackdropBuild = 22621;
constexpr long kWindows10AcrylicBuild = 17134;
constexpr COLORREF kSidebarAcrylicColorKey = RGB(1, 2, 3);

std::mutex g_acrylic_options_mutex;
DesktopAcrylicOptions g_acrylic_options{};
bool g_acrylic_options_initialized = false;

constexpr DWORD kDwmTnpSourceClientAreaOnly = 0x00000010;
constexpr DWORD kDwmTnpVisible = 0x00000008;
constexpr DWORD kDwmTnpRectDestination = 0x00000001;
constexpr DWORD kDwmTnpRectSource = 0x00000002;
constexpr DWORD kDwmTnpOpacity = 0x00000004;
constexpr DWORD kDwmTnpEnable3d = 0x04000000;

RECT virtual_screen_rect() {
    RECT rect{};
    rect.left = ::GetSystemMetrics(SM_XVIRTUALSCREEN);
    rect.top = ::GetSystemMetrics(SM_YVIRTUALSCREEN);
    rect.right = rect.left + std::max(1, ::GetSystemMetrics(SM_CXVIRTUALSCREEN));
    rect.bottom = rect.top + std::max(1, ::GetSystemMetrics(SM_CYVIRTUALSCREEN));
    return rect;
}

SIZE rect_size(RECT rect) {
    return SIZE{
        std::max<LONG>(1, rect.right - rect.left),
        std::max<LONG>(1, rect.bottom - rect.top),
    };
}

std::string hresult_hex(HRESULT hr) {
    std::ostringstream oss;
    oss << "0x" << std::hex << static_cast<unsigned long>(hr);
    return oss.str();
}

using RtlGetVersionPtr = LONG(WINAPI*)(PRTL_OSVERSIONINFOW);

long windows_build_number() {
    HMODULE ntdll = ::GetModuleHandleW(L"ntdll.dll");
    if (!ntdll) return 0;
    auto rtl_get_version =
        reinterpret_cast<RtlGetVersionPtr>(::GetProcAddress(ntdll, "RtlGetVersion"));
    if (!rtl_get_version) return 0;

    RTL_OSVERSIONINFOW version{};
    version.dwOSVersionInfoSize = sizeof(version);
    if (rtl_get_version(&version) != 0) return 0;
    return static_cast<long>(version.dwBuildNumber);
}

enum WindowCompositionAttrib {
    WCA_EXCLUDED_FROM_LIVEPREVIEW = 0x0D,
    WCA_ACCENT_POLICY = 19,
};

struct WindowCompositionAttribData {
    WindowCompositionAttrib Attrib;
    void* pvData;
    DWORD cbData;
};

using SetWindowCompositionAttributePtr =
    BOOL(WINAPI*)(HWND hwnd, WindowCompositionAttribData* data);

enum AccentState {
    ACCENT_DISABLED = 0,
    ACCENT_ENABLE_BLURBEHIND = 3,
    ACCENT_ENABLE_ACRYLICBLURBEHIND = 4,
};

struct AccentPolicy {
    int accent_state;
    int accent_flags;
    int gradient_color;
    int animation_id;
};

bool windows_apps_use_light_theme() {
    DWORD value = 1;
    DWORD size = sizeof(value);
    const LSTATUS status = ::RegGetValueW(HKEY_CURRENT_USER,
                                          L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                                          L"AppsUseLightTheme",
                                          RRF_RT_REG_DWORD,
                                          nullptr,
                                          &value,
                                          &size);
    if (status != ERROR_SUCCESS) return true;
    return value != 0;
}

int clamp_byte(int value) {
    return std::max(0, std::min(255, value));
}

DesktopAcrylicOptions normalize_options(DesktopAcrylicOptions options) {
    options.tint_red = clamp_byte(options.tint_red);
    options.tint_green = clamp_byte(options.tint_green);
    options.tint_blue = clamp_byte(options.tint_blue);
    options.tint_alpha = clamp_byte(options.tint_alpha);
    return options;
}

DesktopAcrylicOptions default_acrylic_options() {
    if (windows_apps_use_light_theme()) {
        return DesktopAcrylicOptions{242, 243, 248, 255};
    }
    return DesktopAcrylicOptions{28, 31, 33, 255};
}

DWORD gradient_color_from_options(DesktopAcrylicOptions options) {
    options = normalize_options(options);
    return (static_cast<DWORD>(options.tint_alpha) << 24) |
           (static_cast<DWORD>(options.tint_blue) << 16) |
           (static_cast<DWORD>(options.tint_green) << 8) |
           static_cast<DWORD>(options.tint_red);
}

bool set_system_backdrop_type(HWND hwnd, int backdrop, std::string& error) {
    HRESULT hr = ::DwmSetWindowAttribute(hwnd,
                                         kDwmWindowAttributeSystemBackdropType,
                                         &backdrop,
                                         sizeof(backdrop));
    if (FAILED(hr)) {
        error = "DwmSetWindowAttribute(DWMWA_SYSTEMBACKDROP_TYPE) failed: " + hresult_hex(hr);
        return false;
    }
    return true;
}

void disable_system_backdrop_if_available(HWND hwnd) {
    if (windows_build_number() < kWindows11SystemBackdropBuild) return;
    std::string ignored;
    (void)set_system_backdrop_type(hwnd, kDwmSystemBackdropNone, ignored);
}

void prefer_rounded_corners_if_available(HWND hwnd) {
    if (windows_build_number() < kWindows11RoundCornersBuild) return;
    int preference = kDwmWindowCornerPreferenceRound;
    HRESULT hr = ::DwmSetWindowAttribute(hwnd,
                                         kDwmWindowAttributeWindowCornerPreference,
                                         &preference,
                                         sizeof(preference));
    if (FAILED(hr)) {
        LOG_DEBUG("[desktop] DwmSetWindowAttribute(DWMWA_WINDOW_CORNER_PREFERENCE) skipped: " +
                  hresult_hex(hr));
    }
}

bool extend_frame_into_client(HWND hwnd, std::string& error) {
    MARGINS margins{-1, -1, -1, -1};
    HRESULT hr = ::DwmExtendFrameIntoClientArea(hwnd, &margins);
    if (FAILED(hr)) {
        error = "DwmExtendFrameIntoClientArea failed: " + hresult_hex(hr);
        return false;
    }
    return true;
}

bool enable_dwm_blur_behind(HWND hwnd, std::string& error) {
    DWM_BLURBEHIND blur{};
    blur.dwFlags = DWM_BB_ENABLE;
    blur.fEnable = TRUE;
    HRESULT hr = ::DwmEnableBlurBehindWindow(hwnd, &blur);
    if (FAILED(hr)) {
        error = "DwmEnableBlurBehindWindow failed: " + hresult_hex(hr);
        return false;
    }
    return true;
}

bool apply_accent_policy(HWND hwnd, int accent_state, DWORD gradient_color, std::string& error) {
    HMODULE user32 = ::GetModuleHandleW(L"user32.dll");
    if (!user32) user32 = ::LoadLibraryW(L"user32.dll");
    if (!user32) {
        error = "user32.dll unavailable";
        return false;
    }

    auto set_window_composition_attribute =
        reinterpret_cast<SetWindowCompositionAttributePtr>(
            ::GetProcAddress(user32, "SetWindowCompositionAttribute"));
    if (!set_window_composition_attribute) {
        error = "SetWindowCompositionAttribute unavailable";
        return false;
    }

    AccentPolicy policy{};
    policy.accent_state = accent_state;
    policy.accent_flags = 2;
    policy.gradient_color = static_cast<int>(gradient_color);
    policy.animation_id = 0;

    WindowCompositionAttribData data{};
    data.Attrib = WCA_ACCENT_POLICY;
    data.pvData = &policy;
    data.cbData = sizeof(policy);
    if (!set_window_composition_attribute(hwnd, &data)) {
        error = "SetWindowCompositionAttribute(WCA_ACCENT_POLICY) failed: " +
                std::to_string(::GetLastError());
        return false;
    }
    return true;
}

bool make_window_frameless_popup(HWND hwnd, std::string& error) {
#  if ACECODE_DESKTOP_ACRYLIC_USE_FRAMELESS_POPUP
    ::SetLastError(ERROR_SUCCESS);
    LONG_PTR style = ::GetWindowLongPtrW(hwnd, GWL_STYLE);
    if (style == 0 && ::GetLastError() != ERROR_SUCCESS) {
        error = "GetWindowLongPtrW(GWL_STYLE) failed: " + std::to_string(::GetLastError());
        return false;
    }

    LONG_PTR next_style = style;
    next_style &= ~(WS_CAPTION | WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_SYSMENU);
    next_style |= WS_POPUP;
    if (next_style != style) {
        ::SetLastError(ERROR_SUCCESS);
        if (::SetWindowLongPtrW(hwnd, GWL_STYLE, next_style) == 0 &&
            ::GetLastError() != ERROR_SUCCESS) {
            error = "SetWindowLongPtrW(GWL_STYLE) failed: " + std::to_string(::GetLastError());
            return false;
        }
    }
#  else
    (void)hwnd;
    (void)error;
#  endif
    return true;
}

bool apply_layered_colorkey(HWND hwnd, std::string& error) {
    ::SetLastError(ERROR_SUCCESS);
    LONG_PTR ex_style = ::GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    if (ex_style == 0 && ::GetLastError() != ERROR_SUCCESS) {
        error = "GetWindowLongPtrW(GWL_EXSTYLE) failed: " + std::to_string(::GetLastError());
        return false;
    }

    LONG_PTR next_ex_style = ex_style | WS_EX_LAYERED;
    if (next_ex_style != ex_style) {
        ::SetLastError(ERROR_SUCCESS);
        if (::SetWindowLongPtrW(hwnd, GWL_EXSTYLE, next_ex_style) == 0 &&
            ::GetLastError() != ERROR_SUCCESS) {
            error = "SetWindowLongPtrW(GWL_EXSTYLE) failed: " + std::to_string(::GetLastError());
            return false;
        }
    }

    if (!::SetLayeredWindowAttributes(hwnd, kSidebarAcrylicColorKey, 255, LWA_COLORKEY)) {
        error = "SetLayeredWindowAttributes(LWA_COLORKEY) failed: " +
                std::to_string(::GetLastError());
        return false;
    }
    return true;
}

bool enable_layered_colorkey(HWND hwnd, std::string& error) {
#  if ACECODE_DESKTOP_ACRYLIC_USE_LAYERED_COLORKEY
    std::string top_error;
    bool top_ok = apply_layered_colorkey(hwnd, top_error);

    struct EnumContext {
        int applied = 0;
        int failed = 0;
    } context;
    ::EnumChildWindows(
        hwnd,
        [](HWND child, LPARAM param) -> BOOL {
            auto* ctx = reinterpret_cast<EnumContext*>(param);
            std::string child_error;
            if (apply_layered_colorkey(child, child_error)) {
                ++ctx->applied;
            } else {
                ++ctx->failed;
            }
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&context));

    if (!top_ok && context.applied == 0) {
        error = top_error.empty() ? "layered color key failed on host and child HWNDs" : top_error;
        return false;
    }
    LOG_DEBUG("[desktop] layered color key applied; top=" + std::string(top_ok ? "true" : "false") +
              " child_applied=" + std::to_string(context.applied) +
              " child_failed=" + std::to_string(context.failed));
#  else
    (void)hwnd;
    (void)error;
#  endif
    return true;
}

bool refresh_window_frame(HWND hwnd, std::string& error) {
    RECT rect{};
    if (!::GetWindowRect(hwnd, &rect)) {
        error = "GetWindowRect failed: " + std::to_string(::GetLastError());
        return false;
    }
    if (!::SetWindowPos(hwnd,
                        nullptr,
                        rect.left,
                        rect.top,
                        rect.right - rect.left,
                        rect.bottom - rect.top,
                        SWP_FRAMECHANGED | SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOMOVE |
                            SWP_NOSIZE)) {
        error = "SetWindowPos(SWP_FRAMECHANGED) failed: " + std::to_string(::GetLastError());
        return false;
    }
    return true;
}

bool prepare_acrylic_window(HWND hwnd, std::string& error) {
    if (!make_window_frameless_popup(hwnd, error)) return false;
    if (!enable_layered_colorkey(hwnd, error)) return false;
    prefer_rounded_corners_if_available(hwnd);
    return refresh_window_frame(hwnd, error);
}

enum class DemoBackdropSource {
    Desktop,
    Host,
};
using DwmpCreateSharedThumbnailVisualPtr =
    HRESULT(WINAPI*)(HWND hwndDestination,
                     HWND hwndSource,
                     DWORD thumbnailFlags,
                     DWM_THUMBNAIL_PROPERTIES* thumbnailProperties,
                     void* dcompDevice,
                     void** visual,
                     PHTHUMBNAIL thumbnailId);
using DwmpCreateSharedMultiWindowVisualPtr =
    HRESULT(WINAPI*)(HWND hwndDestination, void* dcompDevice, void** visual, PHTHUMBNAIL thumbnailId);
using DwmpUpdateSharedMultiWindowVisualPtr =
    HRESULT(WINAPI*)(HTHUMBNAIL thumbnailId,
                     HWND* includeHwnds,
                     DWORD includeCount,
                     HWND* excludeHwnds,
                     DWORD excludeCount,
                     RECT* sourceRect,
                     SIZE* destinationSize,
                     DWORD flags);
using DwmpCreateSharedVirtualDesktopVisualPtr =
    HRESULT(WINAPI*)(HWND hwndDestination, void* dcompDevice, void** visual, PHTHUMBNAIL thumbnailId);
using DwmpUpdateSharedVirtualDesktopVisualPtr =
    HRESULT(WINAPI*)(HTHUMBNAIL thumbnailId,
                     HWND* includeHwnds,
                     DWORD includeCount,
                     HWND* excludeHwnds,
                     DWORD excludeCount,
                     RECT* sourceRect,
                     SIZE* destinationSize);

class DemoAcrylicCompositor {
public:
    bool initialize(HWND hwnd, std::string& error) {
        hwnd_ = hwnd;
        if (!load_functions(error)) return false;
        if (!create_composition_device(error)) return false;
        if (!create_effect_graph(error)) return false;
        if (!create_backdrop(hwnd, DemoBackdropSource::Host, error)) return false;
        if (!create_composition_target(hwnd, error)) return false;
        if (!create_tint_visual(error)) return false;
        arrange_visuals();
        sync_coordinates(hwnd);
        flush_backdrop();
        return true;
    }

    void handle_message(HWND hwnd, UINT msg, WPARAM wparam, LPARAM /*lparam*/) {
        switch (msg) {
            case WM_ACTIVATE:
                active_ = LOWORD(wparam) == WA_ACTIVE || LOWORD(wparam) == WA_CLICKACTIVE;
                sync_tint_visual();
                flush_backdrop();
                break;
            case WM_WINDOWPOSCHANGED:
            case WM_SIZE:
            case WM_DPICHANGED:
                sync_coordinates(hwnd);
                flush_backdrop();
                break;
            case WM_CLOSE:
            case WM_DESTROY:
            case WM_NCDESTROY:
                reset();
                break;
            default:
                break;
        }
    }

    void reset() {
        top_level_thumbnail_ = nullptr;
        desktop_thumbnail_ = nullptr;
        root_visual_.Reset();
        tint_visual_.Reset();
        desktop_visual_.Reset();
        top_level_visual_.Reset();
        target_.Reset();
        swap_chain_.Reset();
        fallback_surface_.Reset();
        fallback_bitmap_.Reset();
        fallback_brush_.Reset();
        device_context_.Reset();
        clip_.Reset();
        translate_transform_.Reset();
        blur_effect_.Reset();
        saturation_effect_.Reset();
    }

private:
    bool load_functions(std::string& error) {
        HMODULE dwmapi = ::LoadLibraryW(L"dwmapi.dll");
        HMODULE user32 = ::LoadLibraryW(L"user32.dll");
        if (!dwmapi || !user32) {
            error = "dwmapi.dll or user32.dll unavailable";
            return false;
        }

        set_window_composition_attribute_ =
            reinterpret_cast<SetWindowCompositionAttributePtr>(
                ::GetProcAddress(user32, "SetWindowCompositionAttribute"));
        create_shared_thumbnail_visual_ =
            reinterpret_cast<DwmpCreateSharedThumbnailVisualPtr>(
                ::GetProcAddress(dwmapi, MAKEINTRESOURCEA(147)));
        create_shared_multi_window_visual_ =
            reinterpret_cast<DwmpCreateSharedMultiWindowVisualPtr>(
                ::GetProcAddress(dwmapi, MAKEINTRESOURCEA(163)));
        update_shared_multi_window_visual_ =
            reinterpret_cast<DwmpUpdateSharedMultiWindowVisualPtr>(
                ::GetProcAddress(dwmapi, MAKEINTRESOURCEA(164)));
        create_shared_virtual_desktop_visual_ =
            reinterpret_cast<DwmpCreateSharedVirtualDesktopVisualPtr>(
                ::GetProcAddress(dwmapi, MAKEINTRESOURCEA(163)));
        update_shared_virtual_desktop_visual_ =
            reinterpret_cast<DwmpUpdateSharedVirtualDesktopVisualPtr>(
                ::GetProcAddress(dwmapi, MAKEINTRESOURCEA(164)));

        if (!create_shared_thumbnail_visual_ ||
            (!create_shared_multi_window_visual_ && !create_shared_virtual_desktop_visual_) ||
            (!update_shared_multi_window_visual_ && !update_shared_virtual_desktop_visual_)) {
            error = "required private DWM acrylic exports unavailable";
            return false;
        }
        return true;
    }

    bool create_composition_device(std::string& error) {
        HRESULT hr = ::D3D11CreateDevice(nullptr,
                                         D3D_DRIVER_TYPE_HARDWARE,
                                         nullptr,
                                         D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                                         nullptr,
                                         0,
                                         D3D11_SDK_VERSION,
                                         d3d_device_.GetAddressOf(),
                                         nullptr,
                                         nullptr);
        if (FAILED(hr)) {
            error = "D3D11CreateDevice failed: " + hresult_hex(hr);
            return false;
        }

        hr = d3d_device_.As(&dxgi_device_);
        if (FAILED(hr)) {
            error = "Query IDXGIDevice failed: " + hresult_hex(hr);
            return false;
        }

        hr = ::D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,
                                 __uuidof(ID2D1Factory2),
                                 reinterpret_cast<void**>(d2d_factory_.GetAddressOf()));
        if (FAILED(hr)) {
            error = "D2D1CreateFactory failed: " + hresult_hex(hr);
            return false;
        }

        hr = d2d_factory_->CreateDevice(dxgi_device_.Get(), d2d_device_.GetAddressOf());
        if (FAILED(hr)) {
            error = "CreateDevice for D2D failed: " + hresult_hex(hr);
            return false;
        }

        hr = ::DCompositionCreateDevice3(dxgi_device_.Get(),
                                         __uuidof(IDCompositionDesktopDevice),
                                         reinterpret_cast<void**>(dcomp_device_.GetAddressOf()));
        if (FAILED(hr)) {
            error = "DCompositionCreateDevice3 failed: " + hresult_hex(hr);
            return false;
        }

        hr = dcomp_device_.As(&dcomp_device3_);
        if (FAILED(hr)) {
            error = "Query IDCompositionDevice3 failed: " + hresult_hex(hr);
            return false;
        }
        return true;
    }

    bool create_effect_graph(std::string& error) {
        HRESULT hr = dcomp_device3_->CreateGaussianBlurEffect(blur_effect_.GetAddressOf());
        if (FAILED(hr)) {
            error = "CreateGaussianBlurEffect failed: " + hresult_hex(hr);
            return false;
        }
        hr = dcomp_device3_->CreateSaturationEffect(saturation_effect_.GetAddressOf());
        if (FAILED(hr)) {
            error = "CreateSaturationEffect failed: " + hresult_hex(hr);
            return false;
        }
        hr = dcomp_device3_->CreateTranslateTransform(translate_transform_.GetAddressOf());
        if (FAILED(hr)) {
            error = "CreateTranslateTransform failed: " + hresult_hex(hr);
            return false;
        }
        hr = dcomp_device3_->CreateRectangleClip(clip_.GetAddressOf());
        if (FAILED(hr)) {
            error = "CreateRectangleClip failed: " + hresult_hex(hr);
            return false;
        }

        saturation_effect_->SetSaturation(2.0f);
        blur_effect_->SetBorderMode(D2D1_BORDER_MODE_HARD);
        blur_effect_->SetInput(0, saturation_effect_.Get(), 0);
        blur_effect_->SetStandardDeviation(40.0f);
        return true;
    }

    bool mark_host_excluded_from_live_preview(HWND hwnd) const {
        if (!set_window_composition_attribute_) return false;
        BOOL enabled = TRUE;
        WindowCompositionAttribData data{};
        data.Attrib = WCA_EXCLUDED_FROM_LIVEPREVIEW;
        data.pvData = &enabled;
        data.cbData = sizeof(enabled);
        return set_window_composition_attribute_(hwnd, &data) != FALSE;
    }

    bool create_backdrop(HWND hwnd, DemoBackdropSource source, std::string& error) {
        switch (source) {
            case DemoBackdropSource::Desktop:
                return create_desktop_backdrop(hwnd, error);
            case DemoBackdropSource::Host:
                return create_host_backdrop(hwnd, error);
        }
        error = "unknown demo acrylic source";
        return false;
    }

    bool create_desktop_backdrop(HWND hwnd, std::string& error) {
        HWND desktop = ::FindWindowW(L"Progman", nullptr);
        if (!desktop) desktop = ::GetShellWindow();
        if (!desktop) {
            error = "desktop window unavailable";
            return false;
        }

        RECT source = virtual_screen_rect();
        SIZE size = rect_size(source);
        DWM_THUMBNAIL_PROPERTIES thumbnail{};
        thumbnail.dwFlags = kDwmTnpSourceClientAreaOnly | kDwmTnpVisible |
                            kDwmTnpRectDestination | kDwmTnpRectSource |
                            kDwmTnpOpacity | kDwmTnpEnable3d;
        thumbnail.opacity = 255;
        thumbnail.fVisible = TRUE;
        thumbnail.fSourceClientAreaOnly = FALSE;
        thumbnail.rcDestination = RECT{0, 0, size.cx, size.cy};
        thumbnail.rcSource = source;

        HRESULT hr = create_shared_thumbnail_visual_(hwnd,
                                                     desktop,
                                                     2,
                                                     &thumbnail,
                                                     dcomp_device_.Get(),
                                                     reinterpret_cast<void**>(desktop_visual_.GetAddressOf()),
                                                     &desktop_thumbnail_);
        if (FAILED(hr)) {
            error = "DwmpCreateSharedThumbnailVisual failed: " + hresult_hex(hr);
            return false;
        }
        return true;
    }

    bool create_host_backdrop(HWND hwnd, std::string& error) {
        HRESULT hr = E_FAIL;
        if (windows_build_number() >= 20000 && create_shared_multi_window_visual_) {
            hr = create_shared_multi_window_visual_(
                hwnd, dcomp_device_.Get(), reinterpret_cast<void**>(top_level_visual_.GetAddressOf()), &top_level_thumbnail_);
        } else if (create_shared_virtual_desktop_visual_) {
            hr = create_shared_virtual_desktop_visual_(
                hwnd, dcomp_device_.Get(), reinterpret_cast<void**>(top_level_visual_.GetAddressOf()), &top_level_thumbnail_);
        }
        if (FAILED(hr)) {
            error = "create host backdrop visual failed: " + hresult_hex(hr);
            return false;
        }
        if (!create_desktop_backdrop(hwnd, error)) return false;

        // Match the reference demo: pass one NULL exclusion entry to the private
        // DWM host-backdrop updater. Passing the host HWND here can produce an
        // empty/black visual on current Windows 11 builds.
        exclude_hwnd_ = nullptr;
        if (!flush_backdrop()) {
            error = "initial host backdrop update failed";
            return false;
        }
        return true;
    }

    bool create_composition_target(HWND hwnd, std::string& error) {
        HRESULT hr = dcomp_device3_->CreateVisual(root_visual_.GetAddressOf());
        if (FAILED(hr)) {
            error = "CreateVisual root failed: " + hresult_hex(hr);
            return false;
        }

        hr = dcomp_device_->CreateTargetForHwnd(hwnd, FALSE, target_.GetAddressOf());
        if (FAILED(hr)) {
            error = "CreateTargetForHwnd failed: " + hresult_hex(hr);
            return false;
        }

        hr = target_->SetRoot(root_visual_.Get());
        if (FAILED(hr)) {
            error = "SetRoot failed: " + hresult_hex(hr);
            return false;
        }
        return true;
    }

    bool create_tint_visual(std::string& error) {
        DXGI_SWAP_CHAIN_DESC1 desc{};
        const SIZE screen_size = rect_size(virtual_screen_rect());
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
        desc.BufferCount = 2;
        desc.SampleDesc.Count = 1;
        desc.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
        desc.Width = static_cast<UINT>(screen_size.cx);
        desc.Height = static_cast<UINT>(screen_size.cy);

        ComPtr<IDXGIFactory2> dxgi_factory;
        HRESULT hr = ::CreateDXGIFactory2(
            0, __uuidof(IDXGIFactory2), reinterpret_cast<void**>(dxgi_factory.GetAddressOf()));
        if (FAILED(hr)) {
            error = "CreateDXGIFactory2 failed: " + hresult_hex(hr);
            return false;
        }

        hr = dxgi_factory->CreateSwapChainForComposition(
            dxgi_device_.Get(), &desc, nullptr, swap_chain_.GetAddressOf());
        if (FAILED(hr)) {
            error = "CreateSwapChainForComposition failed: " + hresult_hex(hr);
            return false;
        }

        hr = d2d_device_->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, device_context_.GetAddressOf());
        if (FAILED(hr)) {
            error = "CreateDeviceContext failed: " + hresult_hex(hr);
            return false;
        }

        hr = swap_chain_->GetBuffer(
            0, __uuidof(IDXGISurface2), reinterpret_cast<void**>(fallback_surface_.GetAddressOf()));
        if (FAILED(hr)) {
            error = "GetBuffer fallback surface failed: " + hresult_hex(hr);
            return false;
        }

        D2D1_BITMAP_PROPERTIES1 properties{};
        properties.pixelFormat.format = DXGI_FORMAT_B8G8R8A8_UNORM;
        properties.pixelFormat.alphaMode = D2D1_ALPHA_MODE_PREMULTIPLIED;
        properties.bitmapOptions = D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW;
        hr = device_context_->CreateBitmapFromDxgiSurface(
            fallback_surface_.Get(), properties, fallback_bitmap_.GetAddressOf());
        if (FAILED(hr)) {
            error = "CreateBitmapFromDxgiSurface failed: " + hresult_hex(hr);
            return false;
        }

        device_context_->SetTarget(fallback_bitmap_.Get());
        hr = device_context_->CreateSolidColorBrush(tint_color_, fallback_brush_.GetAddressOf());
        if (FAILED(hr)) {
            error = "CreateSolidColorBrush failed: " + hresult_hex(hr);
            return false;
        }

        hr = dcomp_device3_->CreateVisual(tint_visual_.GetAddressOf());
        if (FAILED(hr)) {
            error = "CreateVisual tint failed: " + hresult_hex(hr);
            return false;
        }
        hr = tint_visual_->SetContent(swap_chain_.Get());
        if (FAILED(hr)) {
            error = "SetContent tint failed: " + hresult_hex(hr);
            return false;
        }
        sync_tint_visual();
        return true;
    }

    void arrange_visuals() {
        if (!root_visual_) return;
        root_visual_->RemoveAllVisuals();
        if (desktop_visual_) {
            root_visual_->AddVisual(desktop_visual_.Get(), FALSE, nullptr);
        }
        if (top_level_visual_) {
            root_visual_->AddVisual(top_level_visual_.Get(), TRUE, desktop_visual_.Get());
        }
        if (tint_visual_) {
            root_visual_->AddVisual(
                tint_visual_.Get(), TRUE, top_level_visual_ ? top_level_visual_.Get() : desktop_visual_.Get());
        }
        root_visual_->SetEffect(blur_effect_.Get());
        dcomp_device_->Commit();
    }

    void sync_coordinates(HWND hwnd) {
        if (!root_visual_ || !translate_transform_ || !clip_) return;
        RECT client{};
        if (!::GetClientRect(hwnd, &client)) return;
        POINT origin{0, 0};
        if (!::ClientToScreen(hwnd, &origin)) return;

        const RECT screen = virtual_screen_rect();
        const float left = static_cast<float>(origin.x - screen.left);
        const float top = static_cast<float>(origin.y - screen.top);
        const float right = left + static_cast<float>(std::max<LONG>(1, client.right - client.left));
        const float bottom = top + static_cast<float>(std::max<LONG>(1, client.bottom - client.top));
        clip_->SetLeft(left);
        clip_->SetTop(top);
        clip_->SetRight(right);
        clip_->SetBottom(bottom);
        root_visual_->SetClip(clip_.Get());

        translate_transform_->SetOffsetX(-left);
        translate_transform_->SetOffsetY(-top);
        root_visual_->SetTransform(translate_transform_.Get());
        dcomp_device_->Commit();
        ::DwmFlush();
    }

    bool flush_backdrop() {
        if (!top_level_thumbnail_) return true;
        RECT source = virtual_screen_rect();
        SIZE destination = rect_size(source);
        HWND excludes[1] = {exclude_hwnd_};
        HRESULT hr = S_OK;
        if (windows_build_number() >= 20000 && update_shared_multi_window_visual_) {
            hr = update_shared_multi_window_visual_(
                top_level_thumbnail_, nullptr, 0, excludes, 1, &source, &destination, 1);
        } else if (update_shared_virtual_desktop_visual_) {
            hr = update_shared_virtual_desktop_visual_(
                top_level_thumbnail_, nullptr, 0, excludes, 1, &source, &destination);
        }
        if (FAILED(hr)) return false;
        ::DwmFlush();
        return true;
    }

    void sync_tint_visual() {
        if (!device_context_ || !fallback_brush_ || !swap_chain_) return;
        const D2D1_COLOR_F color = active_ ? tint_color_ : fallback_color_;
        const SIZE size = rect_size(virtual_screen_rect());
        fallback_brush_->SetColor(color);
        device_context_->BeginDraw();
        device_context_->Clear();
        D2D1_RECT_F rect = D2D1::RectF(0.0f,
                                       0.0f,
                                       static_cast<float>(size.cx),
                                       static_cast<float>(size.cy));
        device_context_->FillRectangle(rect, fallback_brush_.Get());
        device_context_->EndDraw();
        swap_chain_->Present(1, 0);
    }

    HWND hwnd_ = nullptr;
    HWND exclude_hwnd_ = nullptr;
    bool active_ = true;
    HTHUMBNAIL desktop_thumbnail_ = nullptr;
    HTHUMBNAIL top_level_thumbnail_ = nullptr;
    D2D1_COLOR_F tint_color_ = D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.22f);
    D2D1_COLOR_F fallback_color_ = D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.22f);

    SetWindowCompositionAttributePtr set_window_composition_attribute_ = nullptr;
    DwmpCreateSharedThumbnailVisualPtr create_shared_thumbnail_visual_ = nullptr;
    DwmpCreateSharedMultiWindowVisualPtr create_shared_multi_window_visual_ = nullptr;
    DwmpUpdateSharedMultiWindowVisualPtr update_shared_multi_window_visual_ = nullptr;
    DwmpCreateSharedVirtualDesktopVisualPtr create_shared_virtual_desktop_visual_ = nullptr;
    DwmpUpdateSharedVirtualDesktopVisualPtr update_shared_virtual_desktop_visual_ = nullptr;

    ComPtr<ID3D11Device> d3d_device_;
    ComPtr<IDXGIDevice2> dxgi_device_;
    ComPtr<ID2D1Factory2> d2d_factory_;
    ComPtr<ID2D1Device1> d2d_device_;
    ComPtr<ID2D1DeviceContext> device_context_;
    ComPtr<IDCompositionDesktopDevice> dcomp_device_;
    ComPtr<IDCompositionDevice3> dcomp_device3_;
    ComPtr<IDCompositionTarget> target_;
    ComPtr<IDCompositionVisual2> root_visual_;
    ComPtr<IDCompositionVisual2> tint_visual_;
    ComPtr<IDCompositionVisual2> desktop_visual_;
    ComPtr<IDCompositionVisual2> top_level_visual_;
    ComPtr<IDCompositionGaussianBlurEffect> blur_effect_;
    ComPtr<IDCompositionSaturationEffect> saturation_effect_;
    ComPtr<IDCompositionTranslateTransform> translate_transform_;
    ComPtr<IDCompositionRectangleClip> clip_;
    ComPtr<IDXGISwapChain1> swap_chain_;
    ComPtr<IDXGISurface2> fallback_surface_;
    ComPtr<ID2D1Bitmap1> fallback_bitmap_;
    ComPtr<ID2D1SolidColorBrush> fallback_brush_;
};

std::unique_ptr<DemoAcrylicCompositor> g_demo_compositor;

bool enable_system_backdrop(HWND hwnd, std::string& error) {
    if (windows_build_number() < kWindows11SystemBackdropBuild) {
        error = "Windows build does not support DWMWA_SYSTEMBACKDROP_TYPE";
        return false;
    }
    int backdrop = kDwmSystemBackdropTransientWindow;
    return set_system_backdrop_type(hwnd, backdrop, error);
}

bool enable_accent_acrylic(HWND hwnd, std::string& error) {
    disable_system_backdrop_if_available(hwnd);
    if (!extend_frame_into_client(hwnd, error)) return false;
    std::string blur_error;
    if (!enable_dwm_blur_behind(hwnd, blur_error)) {
        LOG_DEBUG("[desktop] DwmEnableBlurBehindWindow skipped: " + blur_error);
    }

    const int accent_state = windows_build_number() >= kWindows10AcrylicBuild
                                 ? ACCENT_ENABLE_ACRYLICBLURBEHIND
                                 : ACCENT_ENABLE_BLURBEHIND;
    const DWORD tint = gradient_color_from_options(desktop_sidebar_acrylic_options());
    return apply_accent_policy(hwnd, accent_state, tint, error);
}

DesktopAcrylicStatus try_enable_accent(HWND hwnd) {
    DesktopAcrylicStatus status;
    status.backend = DesktopAcrylicBackend::Accent;
    status.enabled = enable_accent_acrylic(hwnd, status.error);
    if (!status.enabled) status.backend = DesktopAcrylicBackend::Off;
    return status;
}

DesktopAcrylicStatus try_enable_system(HWND hwnd) {
    DesktopAcrylicStatus status;
    status.backend = DesktopAcrylicBackend::System;
    status.enabled = enable_system_backdrop(hwnd, status.error);
    if (!status.enabled) status.backend = DesktopAcrylicBackend::Off;
    return status;
}

DesktopAcrylicStatus try_enable_demo(HWND hwnd) {
    DesktopAcrylicStatus status;
    status.backend = DesktopAcrylicBackend::Demo;
    auto compositor = std::make_unique<DemoAcrylicCompositor>();
    status.enabled = compositor->initialize(hwnd, status.error);
    if (status.enabled) {
        g_demo_compositor = std::move(compositor);
    } else {
        status.backend = DesktopAcrylicBackend::Off;
    }
    return status;
}

DesktopAcrylicStatus enable_by_policy(HWND hwnd) {
#  if ACECODE_DESKTOP_ACRYLIC_BACKEND == ACECODE_DESKTOP_ACRYLIC_BACKEND_OFF
    (void)hwnd;
    return DesktopAcrylicStatus{false, DesktopAcrylicBackend::Off, "disabled by compile-time policy"};
#  elif ACECODE_DESKTOP_ACRYLIC_BACKEND == ACECODE_DESKTOP_ACRYLIC_BACKEND_SYSTEM
    return try_enable_system(hwnd);
#  elif ACECODE_DESKTOP_ACRYLIC_BACKEND == ACECODE_DESKTOP_ACRYLIC_BACKEND_DEMO
    return try_enable_demo(hwnd);
#  elif ACECODE_DESKTOP_ACRYLIC_BACKEND == ACECODE_DESKTOP_ACRYLIC_BACKEND_ACCENT
    return try_enable_accent(hwnd);
#  else
    DesktopAcrylicStatus accent = try_enable_accent(hwnd);
    if (accent.enabled) return accent;
    if (windows_build_number() >= kWindows11SystemBackdropBuild) {
        if (!accent.error.empty()) {
            LOG_WARN("[desktop] accent acrylic backend unavailable; falling back to system backdrop: " +
                     accent.error);
        }
        return try_enable_system(hwnd);
    }
    DesktopAcrylicStatus demo = try_enable_demo(hwnd);
    if (!accent.error.empty() && !demo.enabled) {
        demo.error = accent.error + "; demo fallback: " + demo.error;
    }
    return demo;
#  endif
}

} // namespace

DesktopAcrylicStatus enable_desktop_sidebar_acrylic(HWND hwnd) {
    DesktopAcrylicStatus status;
    if (!hwnd || !::IsWindow(hwnd)) {
        status.error = "invalid desktop host HWND";
        return status;
    }

    if (!prepare_acrylic_window(hwnd, status.error)) {
        LOG_WARN("[desktop] sidebar acrylic disabled: " + status.error);
        return status;
    }

    status = enable_by_policy(hwnd);
    if (status.enabled) {
        LOG_INFO(std::string("[desktop] sidebar acrylic enabled with backend=") +
                 desktop_acrylic_backend_name(status.backend));
    } else if (!status.error.empty()) {
        LOG_WARN("[desktop] sidebar acrylic disabled: " + status.error);
    }
    return status;
}

void handle_desktop_sidebar_acrylic_message(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    if (g_demo_compositor) {
        g_demo_compositor->handle_message(hwnd, msg, wparam, lparam);
    }
}

bool set_webview_default_background_transparent(void* browser_controller, std::string* error) {
    if (!browser_controller) {
        if (error) *error = "browser controller unavailable";
        return false;
    }

    auto* controller = static_cast<ICoreWebView2Controller*>(browser_controller);
    ICoreWebView2Controller2* controller2 = nullptr;
    HRESULT hr = controller->QueryInterface(IID_PPV_ARGS(&controller2));
    if (FAILED(hr) || !controller2) {
        if (error) *error = "ICoreWebView2Controller2 unavailable: " + hresult_hex(hr);
        return false;
    }

    COREWEBVIEW2_COLOR transparent{};
    transparent.A = 0;
    transparent.R = 255;
    transparent.G = 255;
    transparent.B = 255;
    hr = controller2->put_DefaultBackgroundColor(transparent);
    controller2->Release();
    if (FAILED(hr)) {
        if (error) *error = "put_DefaultBackgroundColor transparent failed: " + hresult_hex(hr);
        return false;
    }
    LOG_DEBUG("[desktop] WebView2 default background set transparent");
    return true;
}

const char* desktop_acrylic_backend_name(DesktopAcrylicBackend backend) {
    switch (backend) {
        case DesktopAcrylicBackend::Accent: return "accent";
        case DesktopAcrylicBackend::System: return "system";
        case DesktopAcrylicBackend::Demo: return "demo";
        case DesktopAcrylicBackend::Off:
        default: return "off";
    }
}

bool desktop_sidebar_acrylic_prefers_webview_owned_window() {
#  if ACECODE_DESKTOP_ACRYLIC_USE_WEBVIEW_OWNED_WINDOW == 0
    return false;
#  elif ACECODE_DESKTOP_ACRYLIC_BACKEND == ACECODE_DESKTOP_ACRYLIC_BACKEND_OFF
    return false;
#  else
    // The validated Accent Acrylic path uses the webview-created top-level
    // HWND. A separate parent HWND can keep WebView2 opaque even after
    // DefaultBackgroundColor is transparent.
    return true;
#  endif
}

bool desktop_sidebar_acrylic_uses_layered_colorkey() {
#  if ACECODE_DESKTOP_ACRYLIC_USE_LAYERED_COLORKEY == 0
    return false;
#  elif ACECODE_DESKTOP_ACRYLIC_BACKEND == ACECODE_DESKTOP_ACRYLIC_BACKEND_OFF
    return false;
#  else
    return true;
#  endif
}

DesktopAcrylicOptions desktop_sidebar_acrylic_options() {
    std::lock_guard<std::mutex> lock(g_acrylic_options_mutex);
    if (!g_acrylic_options_initialized) {
        return default_acrylic_options();
    }
    return normalize_options(g_acrylic_options);
}

void set_desktop_sidebar_acrylic_options(const DesktopAcrylicOptions& options) {
    std::lock_guard<std::mutex> lock(g_acrylic_options_mutex);
    g_acrylic_options = normalize_options(options);
    g_acrylic_options_initialized = true;
}

} // namespace acecode::desktop

#endif
