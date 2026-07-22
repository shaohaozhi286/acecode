#include "desktop/notifications.hpp"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

namespace {

using acecode::desktop::NotifyPayload;

NotifyPayload sample_payload(std::string id, std::string session_id) {
    NotifyPayload payload;
    payload.id = std::move(id);
    payload.workspace_hash = "workspace-hash";
    payload.session_id = std::move(session_id);
    payload.title = u8"已完成 · 测试";
    payload.body = u8"任务完成";
    return payload;
}

TEST(NativeNotifications, TruncatesOnUnicodeCodepointBoundary) {
    using acecode::desktop::truncate_notification_text;
    EXPECT_EQ(truncate_notification_text("short", 10), "short");
    EXPECT_EQ(truncate_notification_text(u8"中文🙂abc", 3), u8"中文🙂…");
    EXPECT_EQ(truncate_notification_text(u8"中文🙂abc", 6), u8"中文🙂abc");
    EXPECT_TRUE(truncate_notification_text("anything", 0).empty());
}

TEST(NativeNotifications, BuildsCompletionPayloadWithStableSessionIdentity) {
    const auto payload = acecode::desktop::build_completion_notification(
        "session-42", "workspace-7", u8" 修复通知 ", u8" 已经完成 ");
    EXPECT_EQ(payload.workspace_hash, "workspace-7");
    EXPECT_EQ(payload.session_id, "session-42");
    EXPECT_EQ(payload.title, u8"已完成 · 修复通知");
    EXPECT_EQ(payload.body, u8"已经完成");
    EXPECT_NE(payload.id.find("completion-session-42-"), std::string::npos);
}

TEST(NativeNotifications, LocalizesNotificationShellWithoutChangingSessionTitle) {
    const auto payload = acecode::desktop::build_completion_notification(
        "session-en", "workspace", u8"用户标题", "", "en-US");
    EXPECT_EQ(payload.title, u8"Completed · 用户标题");
    EXPECT_EQ(payload.body, "(blank turn)");
}

TEST(NativeNotifications, ParsesDirectObjectBridgeArgument) {
    const auto payload = sample_payload("completion-1", "session-1");
    nlohmann::json object = {
        {"id", payload.id},
        {"workspace_hash", payload.workspace_hash},
        {"session_id", payload.session_id},
        {"title", payload.title},
        {"body", payload.body},
    };
    std::string error;
    auto parsed = acecode::desktop::parse_notification_bridge_args(
        nlohmann::json::array({object}).dump(), &error);
    ASSERT_TRUE(parsed.has_value()) << error;
    EXPECT_EQ(parsed->id, payload.id);
    EXPECT_EQ(parsed->workspace_hash, payload.workspace_hash);
    EXPECT_EQ(parsed->session_id, payload.session_id);
    EXPECT_EQ(parsed->title, payload.title);
    EXPECT_EQ(parsed->body, payload.body);
}

TEST(NativeNotifications, ParsesLegacyJsonStringBridgeArgument) {
    nlohmann::json object = {
        {"id", "question-1"},
        {"workspace_hash", "workspace-2"},
        {"session_id", "session-2"},
        {"title", u8"需要你回答 · 会话"},
        {"body", u8"请选择"},
    };
    const auto args = nlohmann::json::array({object.dump()}).dump();
    std::string error;
    auto parsed = acecode::desktop::parse_notification_bridge_args(args, &error);
    ASSERT_TRUE(parsed.has_value()) << error;
    EXPECT_EQ(parsed->id, "question-1");
    EXPECT_EQ(parsed->session_id, "session-2");
    EXPECT_EQ(parsed->body, u8"请选择");
}

TEST(NativeNotifications, RejectsInvalidOrEmptyBridgePayload) {
    std::string error;
    EXPECT_FALSE(acecode::desktop::parse_notification_bridge_args("[]", &error));
    EXPECT_FALSE(error.empty());
    EXPECT_FALSE(acecode::desktop::parse_notification_bridge_args(
        R"([{"id":"completion-1"}])", &error));
    EXPECT_FALSE(error.empty());
}

TEST(NativeNotifications, ActivationKeepsEachPayloadIndependent) {
    std::vector<std::string> activated;
    acecode::desktop::set_click_handler(
        [&](const NotifyPayload& payload) { activated.push_back(payload.session_id); });

    const auto older = sample_payload("completion-old", "session-old");
    const auto newer = sample_payload("completion-new", "session-new");
    acecode::desktop::dispatch_notification_activation(newer);
    acecode::desktop::dispatch_notification_activation(older);

    ASSERT_EQ(activated.size(), 2u);
    EXPECT_EQ(activated[0], "session-new");
    EXPECT_EQ(activated[1], "session-old");
    acecode::desktop::shutdown_notifications();
}

TEST(NativeNotifications, DeliveryBeforeInitializationIsSafeNoOp) {
    acecode::desktop::shutdown_notifications();
    EXPECT_FALSE(acecode::desktop::show_notification(
        sample_payload("completion-no-init", "session-no-init")));
    EXPECT_FALSE(
        acecode::desktop::refresh_notification_authorization());
    EXPECT_FALSE(acecode::desktop::activate_notification_window(nullptr));
}

TEST(NativeNotifications, AuthorizationStatusNamesAreStable) {
    using Status = acecode::desktop::NotificationAuthorizationStatus;
    EXPECT_STREQ(acecode::desktop::notification_authorization_status_name(
                     Status::Unknown),
                 "unknown");
    EXPECT_STREQ(acecode::desktop::notification_authorization_status_name(
                     Status::NotDetermined),
                 "not_determined");
    EXPECT_STREQ(acecode::desktop::notification_authorization_status_name(
                     Status::Requesting),
                 "requesting");
    EXPECT_STREQ(acecode::desktop::notification_authorization_status_name(
                     Status::Denied),
                 "denied");
    EXPECT_STREQ(acecode::desktop::notification_authorization_status_name(
                     Status::Authorized),
                 "authorized");
    EXPECT_STREQ(acecode::desktop::notification_authorization_status_name(
                     Status::Provisional),
                 "provisional");
    EXPECT_STREQ(acecode::desktop::notification_authorization_status_name(
                     Status::Unavailable),
                 "unavailable");
}

TEST(NativeNotifications, AuthorizationStateResetsToUnavailableOnShutdown) {
    using acecode::desktop::NotificationAuthorizationStatus;
    acecode::desktop::shutdown_notifications();
    const auto state =
        acecode::desktop::notification_authorization_state();
    EXPECT_EQ(state.status, NotificationAuthorizationStatus::Unavailable);
    EXPECT_FALSE(state.can_request);
    EXPECT_FALSE(state.can_open_settings);
}

TEST(NativeNotifications, OptInDeliversWindowsToastAndActivatesWindow) {
#if defined(_WIN32) && !defined(ACECODE_NOTIFICATION_BACKEND_STUB)
    const char* enabled = std::getenv("ACECODE_RUN_NOTIFICATION_SMOKE");
    if (!enabled || std::string(enabled) != "1") {
        GTEST_SKIP() << "set ACECODE_RUN_NOTIFICATION_SMOKE=1 for the Windows runtime smoke";
    }

    acecode::desktop::shutdown_notifications();
    void* window = acecode::desktop::capture_tui_notification_window();
    acecode::desktop::NotificationInitOptions options;
    options.app_name = "ACECode Desktop";
    options.application_id = "ACECode.ACECode.Desktop.1";
    options.activation_window = window;
    ASSERT_TRUE(acecode::desktop::init_notifications(options));

    const auto payload = sample_payload(
        "completion-runtime-smoke", "session-runtime-smoke");
    EXPECT_TRUE(acecode::desktop::show_notification(payload));
    if (window) {
        EXPECT_TRUE(acecode::desktop::activate_notification_window(window));
    }

    // Give Windows Notification Center enough time to accept and render the
    // asynchronous toast before clearing WinToast state.
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));
    acecode::desktop::shutdown_notifications();
#else
    GTEST_SKIP() << "WinToast runtime smoke requires a Windows SDK build";
#endif
}

} // namespace
