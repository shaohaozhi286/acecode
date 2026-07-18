#include "notifications_backend.hpp"

#ifdef _WIN32

#include "../config/config.hpp"
#include "../utils/atomic_file.hpp"
#include "../utils/encoding.hpp"
#include "../utils/logger.hpp"
#include "../utils/utf8_path.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#  define NOMINMAX
#endif
#include <windows.h>
#include <wintoastlib.h>

#include <exception>
#include <filesystem>
#include <mutex>
#include <string>
#include <utility>

namespace acecode::desktop::notification_backend {
namespace {

std::mutex g_backend_mu;
bool g_initialized = false;
std::wstring g_notification_logo_path;

constexpr int kNotificationLogoResourceId = 101;
constexpr int kRawDataResourceType = 10; // RT_RCDATA, wide-resource form

std::wstring materialize_notification_logo() {
    HMODULE module = ::GetModuleHandleW(nullptr);
    HRSRC resource = ::FindResourceW(
        module,
        MAKEINTRESOURCEW(kNotificationLogoResourceId),
        MAKEINTRESOURCEW(kRawDataResourceType));
    if (!resource) {
        LOG_WARN("[notifications] ACECode logo resource was not found");
        return {};
    }
    HGLOBAL loaded = ::LoadResource(module, resource);
    const DWORD size = ::SizeofResource(module, resource);
    const void* bytes = loaded ? ::LockResource(loaded) : nullptr;
    if (!bytes || size == 0) {
        LOG_WARN("[notifications] ACECode logo resource could not be loaded");
        return {};
    }

    const auto path = acecode::path_from_utf8(acecode::get_acecode_dir())
        / "cache" / "acecode-notification-logo.png";
    std::error_code ec;
    if (std::filesystem::exists(path, ec) && !ec &&
        std::filesystem::file_size(path, ec) == size && !ec) {
        return path.wstring();
    }

    const std::string content(
        static_cast<const char*>(bytes),
        static_cast<std::size_t>(size));
    if (!acecode::atomic_write_file(acecode::path_to_utf8(path), content)) {
        LOG_WARN("[notifications] failed to materialize ACECode logo at " +
                 acecode::path_to_utf8(path));
        return {};
    }
    return path.wstring();
}

std::string wintoast_error_text(WinToastLib::WinToast::WinToastError error) {
    try {
        return acecode::wide_to_utf8(WinToastLib::WinToast::strerror(error));
    } catch (...) {
        return std::to_string(static_cast<int>(error));
    }
}

class SessionToastHandler final : public WinToastLib::IWinToastHandler {
public:
    explicit SessionToastHandler(NotifyPayload payload)
        : payload_(std::move(payload)) {}

    void toastActivated() const override {
        dispatch_notification_activation(payload_);
    }

    void toastActivated(int /*action_index*/) const override {
        dispatch_notification_activation(payload_);
    }

    void toastActivated(std::wstring /*response*/) const override {
        dispatch_notification_activation(payload_);
    }

    void toastDismissed(WinToastDismissalReason /*state*/) const override {}

    void toastFailed() const override {
        LOG_WARN("[notifications] WinToast reported delivery failure for " +
                 payload_.id);
    }

private:
    NotifyPayload payload_;
};

} // namespace

bool initialize(const NotificationInitOptions& options) {
    std::lock_guard<std::mutex> lock(g_backend_mu);
    if (g_initialized) return true;
    if (options.application_id.empty()) {
        LOG_WARN("[notifications] WinToast initialization skipped: empty AUMI");
        return false;
    }
    if (!WinToastLib::WinToast::isCompatible()) {
        LOG_WARN("[notifications] WinToast is not compatible with this Windows version");
        return false;
    }

    try {
        WinToastLib::setDebugOutputEnabled(false);
        auto* toast = WinToastLib::WinToast::instance();
        toast->setAppName(acecode::utf8_to_wide(
            options.app_name.empty() ? std::string("ACECode") :
                                       options.app_name));
        toast->setAppUserModelId(
            acecode::utf8_to_wide(options.application_id));
        toast->setShortcutPolicy(
            WinToastLib::WinToast::ShortcutPolicy::SHORTCUT_POLICY_REQUIRE_CREATE);

        WinToastLib::WinToast::WinToastError error =
            WinToastLib::WinToast::WinToastError::NoError;
        if (!toast->initialize(&error)) {
            LOG_WARN("[notifications] WinToast initialization failed: " +
                     wintoast_error_text(error));
            return false;
        }
    } catch (const std::exception& e) {
        LOG_WARN("[notifications] WinToast initialization threw: " +
                 std::string(e.what()));
        return false;
    }

    g_notification_logo_path = materialize_notification_logo();
    g_initialized = true;
    notification_detail::publish_authorization_state({
        NotificationAuthorizationStatus::Authorized, false, false});
    LOG_INFO("[notifications] WinToast initialized" +
             std::string(g_notification_logo_path.empty()
                 ? " without app logo override"
                 : " with ACECode app logo"));
    return true;
}

bool show(const NotifyPayload& payload) {
    std::lock_guard<std::mutex> lock(g_backend_mu);
    if (!g_initialized) return false;

    try {
        WinToastLib::WinToastTemplate toast(
            WinToastLib::WinToastTemplate::WinToastTemplateType::Text02);
        toast.setFirstLine(acecode::utf8_to_wide(
            payload.title.empty() ? std::string("ACECode") : payload.title));
        toast.setSecondLine(acecode::utf8_to_wide(payload.body));
        toast.setDuration(WinToastLib::WinToastTemplate::Duration::Short);
        if (!g_notification_logo_path.empty()) {
            toast.setImagePath(
                g_notification_logo_path,
                WinToastLib::WinToastTemplate::CropHint::Square);
        }

        WinToastLib::WinToast::WinToastError error =
            WinToastLib::WinToast::WinToastError::NoError;
        const auto toast_id = WinToastLib::WinToast::instance()->showToast(
            toast, new SessionToastHandler(payload), &error);
        if (toast_id < 0) {
            LOG_WARN("[notifications] WinToast delivery failed: " +
                     wintoast_error_text(error));
            return false;
        }
        return true;
    } catch (const std::exception& e) {
        LOG_WARN("[notifications] WinToast delivery threw: " +
                 std::string(e.what()));
        return false;
    } catch (...) {
        LOG_WARN("[notifications] WinToast delivery threw an unknown exception");
        return false;
    }
}

void shutdown() {
    bool was_initialized = false;
    {
        std::lock_guard<std::mutex> lock(g_backend_mu);
        was_initialized = g_initialized;
        g_initialized = false;
        g_notification_logo_path.clear();
    }
    if (was_initialized) {
        try {
            WinToastLib::WinToast::instance()->clear();
        } catch (...) {
            LOG_WARN("[notifications] WinToast shutdown failed");
        }
    }
}

bool refresh_authorization() {
    std::lock_guard<std::mutex> lock(g_backend_mu);
    return g_initialized;
}

bool request_authorization() {
    std::lock_guard<std::mutex> lock(g_backend_mu);
    return g_initialized;
}

bool open_settings() {
    return false;
}

void* capture_tui_window() {
    HWND console = ::GetConsoleWindow();
    if (console && ::IsWindow(console) && ::IsWindowVisible(console)) {
        return console;
    }
    HWND foreground = ::GetForegroundWindow();
    if (foreground && ::IsWindow(foreground)) return foreground;
    return console && ::IsWindow(console) ? console : nullptr;
}

bool window_is_foreground(void* native_window) {
    auto* hwnd = static_cast<HWND>(native_window);
    return hwnd && ::IsWindow(hwnd) && ::GetForegroundWindow() == hwnd;
}

bool activate_window(void* native_window) {
    auto* hwnd = static_cast<HWND>(native_window);
    if (!hwnd || !::IsWindow(hwnd)) return false;
    if (::IsIconic(hwnd)) {
        ::ShowWindowAsync(hwnd, SW_RESTORE);
    } else {
        ::ShowWindowAsync(hwnd, SW_SHOW);
    }
    ::BringWindowToTop(hwnd);
    if (::SetForegroundWindow(hwnd)) return true;

    constexpr UINT flags = SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW;
    ::SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, flags);
    ::SetWindowPos(hwnd, HWND_NOTOPMOST, 0, 0, 0, 0, flags);
    return ::SetForegroundWindow(hwnd) != FALSE ||
           ::GetForegroundWindow() == hwnd;
}

} // namespace acecode::desktop::notification_backend

#endif // _WIN32
