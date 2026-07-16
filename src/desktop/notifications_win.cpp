#include "notifications_win.hpp"

#include "../utils/encoding.hpp"
#include "../utils/logger.hpp"

#include <nlohmann/json.hpp>

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#  include <wintoastlib.h>
#endif

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdint>
#include <mutex>
#include <utility>

namespace acecode::desktop {
namespace {

std::mutex g_notification_mu;
ClickHandler g_click_handler;
void* g_activation_window = nullptr;
bool g_initialized = false;
std::atomic<std::uint64_t> g_notification_sequence{0};

void set_parse_error(std::string* error, std::string message) {
    if (error) *error = std::move(message);
}

std::string trim_ascii(std::string value) {
    auto is_space = [](unsigned char ch) { return std::isspace(ch) != 0; };
    value.erase(value.begin(), std::find_if(value.begin(), value.end(),
                                            [&](char ch) {
                                                return !is_space(
                                                    static_cast<unsigned char>(ch));
                                            }));
    value.erase(std::find_if(value.rbegin(), value.rend(),
                             [&](char ch) {
                                 return !is_space(static_cast<unsigned char>(ch));
                             }).base(),
                value.end());
    return value;
}

std::size_t utf8_codepoint_bytes(const std::string& text, std::size_t offset) {
    const auto lead = static_cast<unsigned char>(text[offset]);
    std::size_t length = 1;
    if ((lead & 0xE0u) == 0xC0u) length = 2;
    else if ((lead & 0xF0u) == 0xE0u) length = 3;
    else if ((lead & 0xF8u) == 0xF0u) length = 4;
    if (offset + length > text.size()) return 1;
    for (std::size_t i = 1; i < length; ++i) {
        const auto next = static_cast<unsigned char>(text[offset + i]);
        if ((next & 0xC0u) != 0x80u) return 1;
    }
    return length;
}

#ifdef _WIN32

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

#endif

} // namespace

std::string truncate_notification_text(const std::string& text,
                                       std::size_t max_codepoints) {
    if (text.empty() || max_codepoints == 0) return {};
    std::size_t offset = 0;
    std::size_t count = 0;
    while (offset < text.size() && count < max_codepoints) {
        offset += utf8_codepoint_bytes(text, offset);
        ++count;
    }
    if (offset >= text.size()) return text;
    return text.substr(0, offset) + u8"…";
}

NotifyPayload build_completion_notification(
    const std::string& session_id,
    const std::string& workspace_hash,
    const std::string& session_title,
    const std::string& final_assistant_text) {
    NotifyPayload payload;
    payload.id = "completion-" +
        (session_id.empty() ? std::string("unknown") : session_id) + "-" +
        std::to_string(g_notification_sequence.fetch_add(1) + 1);
    payload.workspace_hash = workspace_hash;
    payload.session_id = session_id;
    const std::string title = trim_ascii(session_title);
    payload.title = u8"已完成 · " + (title.empty() ? std::string(u8"会话") : title);
    const std::string body = trim_ascii(final_assistant_text);
    payload.body = body.empty()
        ? std::string(u8"(空白回合)")
        : truncate_notification_text(body);
    return payload;
}

std::optional<NotifyPayload> parse_notification_bridge_args(
    const std::string& args_json,
    std::string* error) {
    if (error) error->clear();
    try {
        nlohmann::json value = nlohmann::json::parse(args_json);
        if (value.is_array()) {
            if (value.empty()) {
                set_parse_error(error, "notification arguments are empty");
                return std::nullopt;
            }
            value = value.front();
        }
        if (value.is_string()) {
            value = nlohmann::json::parse(value.get<std::string>());
        }
        if (!value.is_object()) {
            set_parse_error(error, "notification payload must be an object");
            return std::nullopt;
        }

        NotifyPayload payload;
        auto copy_string = [&](const char* key, std::string& out) {
            if (value.contains(key) && value[key].is_string()) {
                out = value[key].get<std::string>();
            }
        };
        copy_string("id", payload.id);
        copy_string("workspace_hash", payload.workspace_hash);
        copy_string("session_id", payload.session_id);
        copy_string("title", payload.title);
        copy_string("body", payload.body);
        if (payload.title.empty() && payload.body.empty()) {
            set_parse_error(error, "notification title and body are empty");
            return std::nullopt;
        }
        return payload;
    } catch (const std::exception& e) {
        set_parse_error(error, e.what());
        return std::nullopt;
    }
}

void set_click_handler(ClickHandler handler) {
    std::lock_guard<std::mutex> lock(g_notification_mu);
    g_click_handler = std::move(handler);
}

void dispatch_notification_activation(const NotifyPayload& payload) {
    ClickHandler handler;
    void* activation_window = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_notification_mu);
        handler = g_click_handler;
        activation_window = g_activation_window;
    }
    if (!handler) return;
    activate_notification_window(activation_window);
    handler(payload);
}

#ifdef _WIN32

bool init_notifications(const NotificationInitOptions& options) {
    std::lock_guard<std::mutex> lock(g_notification_mu);
    if (g_initialized) return true;
    if (options.app_user_model_id.empty()) {
        LOG_WARN("[notifications] WinToast initialization skipped: empty AUMI");
        return false;
    }
    if (!WinToastLib::WinToast::isCompatible()) {
        LOG_WARN("[notifications] WinToast is not compatible with this Windows version");
        return false;
    }

    // WinToast writes diagnostics to std::wcout in debug builds. That corrupts
    // the FTXUI frame, so all diagnostics go through ACECode's logger instead.
    WinToastLib::setDebugOutputEnabled(false);
    auto* toast = WinToastLib::WinToast::instance();
    toast->setAppName(options.app_name.empty() ? L"ACECode" : options.app_name);
    toast->setAppUserModelId(options.app_user_model_id);
    toast->setShortcutPolicy(
        WinToastLib::WinToast::ShortcutPolicy::SHORTCUT_POLICY_REQUIRE_CREATE);

    WinToastLib::WinToast::WinToastError error =
        WinToastLib::WinToast::WinToastError::NoError;
    if (!toast->initialize(&error)) {
        LOG_WARN("[notifications] WinToast initialization failed: " +
                 wintoast_error_text(error));
        return false;
    }

    g_activation_window = options.activation_window;
    g_initialized = true;
    LOG_INFO("[notifications] WinToast initialized");
    return true;
}

bool show_notification(const NotifyPayload& payload) {
    std::lock_guard<std::mutex> lock(g_notification_mu);
    if (!g_initialized) return false;
    if (payload.title.empty() && payload.body.empty()) return false;

    try {
        WinToastLib::WinToastTemplate toast(
            WinToastLib::WinToastTemplate::WinToastTemplateType::Text02);
        toast.setFirstLine(acecode::utf8_to_wide(
            payload.title.empty() ? std::string("ACECode") : payload.title));
        toast.setSecondLine(acecode::utf8_to_wide(payload.body));
        toast.setDuration(WinToastLib::WinToastTemplate::Duration::Short);

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

void shutdown_notifications() {
    bool was_initialized = false;
    {
        std::lock_guard<std::mutex> lock(g_notification_mu);
        was_initialized = g_initialized;
        g_initialized = false;
        g_click_handler = nullptr;
        g_activation_window = nullptr;
    }
    if (was_initialized) {
        try {
            WinToastLib::WinToast::instance()->clear();
        } catch (...) {
            LOG_WARN("[notifications] WinToast shutdown failed");
        }
    }
}

void* capture_tui_notification_window() {
    HWND console = ::GetConsoleWindow();
    if (console && ::IsWindow(console) && ::IsWindowVisible(console)) {
        return console;
    }
    HWND foreground = ::GetForegroundWindow();
    if (foreground && ::IsWindow(foreground)) return foreground;
    return console && ::IsWindow(console) ? console : nullptr;
}

bool notification_window_is_foreground(void* native_window) {
    auto* hwnd = static_cast<HWND>(native_window);
    return hwnd && ::IsWindow(hwnd) && ::GetForegroundWindow() == hwnd;
}

bool activate_notification_window(void* native_window) {
    auto* hwnd = static_cast<HWND>(native_window);
    if (!hwnd || !::IsWindow(hwnd)) return false;
    if (::IsIconic(hwnd)) {
        ::ShowWindowAsync(hwnd, SW_RESTORE);
    } else {
        ::ShowWindowAsync(hwnd, SW_SHOW);
    }
    ::BringWindowToTop(hwnd);
    if (::SetForegroundWindow(hwnd)) return true;

    // Foreground lock can still reject background callers. A short topmost
    // pulse makes the user-initiated target visible without leaving it pinned.
    constexpr UINT flags = SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW;
    ::SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, flags);
    ::SetWindowPos(hwnd, HWND_NOTOPMOST, 0, 0, 0, 0, flags);
    return ::SetForegroundWindow(hwnd) != FALSE ||
           ::GetForegroundWindow() == hwnd;
}

#else

bool init_notifications(const NotificationInitOptions& /*options*/) {
    return false;
}

bool show_notification(const NotifyPayload& /*payload*/) {
    return false;
}

void shutdown_notifications() {
    std::lock_guard<std::mutex> lock(g_notification_mu);
    g_initialized = false;
    g_click_handler = nullptr;
    g_activation_window = nullptr;
}

void* capture_tui_notification_window() { return nullptr; }
bool notification_window_is_foreground(void* /*native_window*/) { return false; }
bool activate_notification_window(void* /*native_window*/) { return false; }

#endif

} // namespace acecode::desktop
