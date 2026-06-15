#include <gtest/gtest.h>

#include "hooks/hook_manager.hpp"
#include "hooks/hook_runtime.hpp"

#include <string>
#include <vector>

namespace {

acecode::NormalizedHook make_hook(std::string id,
                                  std::string event,
                                  std::string matcher,
                                  acecode::HookTrustStatus trust =
                                      acecode::HookTrustStatus::Trusted) {
    acecode::NormalizedHook hook;
    hook.id = std::move(id);
    hook.source_id = "source";
    hook.event_name = std::move(event);
    hook.matcher = std::move(matcher);
    hook.kind = acecode::HookHandlerKind::Command;
    hook.command.command = "hook-command";
    hook.command.timeout_seconds = 1;
    hook.trust_status = trust;
    return hook;
}

acecode::HookProcessResult ok_json_result(const std::string& stdout_text) {
    acecode::HookProcessResult result;
    result.started = true;
    result.exit_code = 0;
    result.stdout_text = stdout_text;
    result.output = stdout_text;
    return result;
}

} // namespace

TEST(HookRuntime, MatcherAliasesMapCodexNamesToAceCodeTools) {
    EXPECT_TRUE(acecode::hook_matcher_matches(
        make_hook("h1", acecode::kCodexHookEventPreToolUse, "Bash"),
        acecode::kCodexHookEventPreToolUse,
        "bash"));
    EXPECT_TRUE(acecode::hook_matcher_matches(
        make_hook("h2", acecode::kCodexHookEventPreToolUse, "Edit"),
        acecode::kCodexHookEventPreToolUse,
        "file_edit"));
    EXPECT_TRUE(acecode::hook_matcher_matches(
        make_hook("h3", acecode::kCodexHookEventPreToolUse, "Write"),
        acecode::kCodexHookEventPreToolUse,
        "file_write"));
    EXPECT_TRUE(acecode::hook_matcher_matches(
        make_hook("h4", acecode::kCodexHookEventPreToolUse, "apply_patch"),
        acecode::kCodexHookEventPreToolUse,
        "file_write"));
}

TEST(HookRuntime, InvalidRegexProducesDiagnostic) {
    std::vector<acecode::HookDiagnostic> diagnostics;
    EXPECT_FALSE(acecode::hook_matcher_matches(
        make_hook("h1", acecode::kCodexHookEventPreToolUse, "["),
        acecode::kCodexHookEventPreToolUse,
        "bash",
        &diagnostics));
    ASSERT_EQ(diagnostics.size(), 1u);
    EXPECT_EQ(diagnostics[0].code, "HOOK_MATCHER_INVALID_REGEX");
}

TEST(HookRuntime, CommonPayloadIncludesCodexFields) {
    acecode::HookCommonPayloadFields fields;
    fields.session_id = "sid";
    fields.transcript_path = "session.jsonl";
    fields.cwd = "C:/repo";
    fields.hook_event_name = acecode::kCodexHookEventSessionStart;
    fields.model = "gpt-test";
    fields.permission_mode = "default";
    fields.turn_id = "turn";

    auto payload = acecode::build_session_start_hook_payload(fields, "startup");

    EXPECT_EQ(payload["session_id"], "sid");
    EXPECT_EQ(payload["transcript_path"], "session.jsonl");
    EXPECT_EQ(payload["cwd"], "C:/repo");
    EXPECT_EQ(payload["hook_event_name"], acecode::kCodexHookEventSessionStart);
    EXPECT_EQ(payload["model"], "gpt-test");
    EXPECT_EQ(payload["permission_mode"], "default");
    EXPECT_EQ(payload["turn_id"], "turn");
    EXPECT_EQ(payload["source"], "startup");
}

TEST(HookRuntime, EventPayloadBuildersIncludeEventSpecificFields) {
    acecode::HookCommonPayloadFields fields;
    fields.session_id = "sid";
    fields.cwd = "C:/repo";
    fields.hook_event_name = "event";
    fields.model = "model";
    fields.permission_mode = "default";
    fields.turn_id = "turn";

    auto prompt = acecode::build_user_prompt_submit_hook_payload(fields, "hello");
    EXPECT_EQ(prompt["prompt"], "hello");

    auto tool = acecode::build_tool_hook_payload(
        fields,
        "bash",
        nlohmann::json{{"command", "echo hi"}},
        nlohmann::json{{"output", "hi"}, {"success", true}});
    EXPECT_EQ(tool["tool_name"], "bash");
    EXPECT_EQ(tool["tool_input"]["command"], "echo hi");
    EXPECT_EQ(tool["tool_response"]["output"], "hi");

    auto compact = acecode::build_compact_hook_payload(fields, "manual");
    EXPECT_EQ(compact["trigger"], "manual");

    auto stop = acecode::build_stop_hook_payload(fields, true, "last message");
    EXPECT_EQ(stop["stop_hook_active"], true);
    EXPECT_EQ(stop["last_assistant_message"], "last message");
}

TEST(HookRuntime, ExitCodeTwoBlocksWithStderrReason) {
    acecode::HookProcessResult result;
    result.started = true;
    result.exit_code = 2;
    result.stderr_text = "blocked by policy";

    auto parsed = acecode::parse_hook_process_output(
        result, acecode::kCodexHookEventUserPromptSubmit);
    acecode::HookAggregateOutcome aggregate;
    acecode::merge_hook_output(
        aggregate,
        parsed,
        acecode::kCodexHookEventUserPromptSubmit,
        make_hook("h1", acecode::kCodexHookEventUserPromptSubmit, "*"));

    EXPECT_TRUE(aggregate.blocked);
    EXPECT_TRUE(aggregate.denied);
    EXPECT_EQ(aggregate.reason, "blocked by policy");
}

TEST(HookRuntime, PlainStdoutBecomesContextOnlyForSupportedEvents) {
    auto session_parsed = acecode::parse_hook_process_output(
        ok_json_result("plain context"), acecode::kCodexHookEventSessionStart);
    acecode::HookAggregateOutcome session_out;
    acecode::merge_hook_output(
        session_out,
        session_parsed,
        acecode::kCodexHookEventSessionStart,
        make_hook("h1", acecode::kCodexHookEventSessionStart, "*"));
    ASSERT_EQ(session_out.additional_context.size(), 1u);
    EXPECT_EQ(session_out.additional_context[0], "plain context");

    auto tool_parsed = acecode::parse_hook_process_output(
        ok_json_result("plain ignored"), acecode::kCodexHookEventPreToolUse);
    acecode::HookAggregateOutcome tool_out;
    acecode::merge_hook_output(
        tool_out,
        tool_parsed,
        acecode::kCodexHookEventPreToolUse,
        make_hook("h2", acecode::kCodexHookEventPreToolUse, "*"));
    EXPECT_TRUE(tool_out.additional_context.empty());
}

TEST(HookRuntime, JsonOutputMergesPermissionDecisionAndAdditionalContext) {
    auto parsed = acecode::parse_hook_process_output(
        ok_json_result(R"({
            "hookSpecificOutput": {
                "permissionDecision": "deny",
                "permissionDecisionReason": "no rm",
                "additionalContext": "remember this"
            },
            "systemMessage": "visible warning"
        })"),
        acecode::kCodexHookEventPermissionRequest);

    acecode::HookAggregateOutcome aggregate;
    acecode::merge_hook_output(
        aggregate,
        parsed,
        acecode::kCodexHookEventPermissionRequest,
        make_hook("h1", acecode::kCodexHookEventPermissionRequest, "Bash"));

    EXPECT_TRUE(aggregate.denied);
    EXPECT_FALSE(aggregate.allowed);
    EXPECT_EQ(aggregate.reason, "no rm");
    ASSERT_EQ(aggregate.additional_context.size(), 1u);
    EXPECT_EQ(aggregate.additional_context[0], "remember this");
    ASSERT_EQ(aggregate.system_messages.size(), 1u);
    EXPECT_EQ(aggregate.system_messages[0], "visible warning");
}

TEST(HookRuntime, ContinueFalseTakesPrecedenceOverAllowAndCapturesReason) {
    auto parsed = acecode::parse_hook_process_output(
        ok_json_result(R"({
            "continue": false,
            "decision": "allow",
            "reason": "stop now"
        })"),
        acecode::kCodexHookEventStop);

    acecode::HookAggregateOutcome aggregate;
    acecode::merge_hook_output(
        aggregate,
        parsed,
        acecode::kCodexHookEventStop,
        make_hook("h1", acecode::kCodexHookEventStop, "*"));

    EXPECT_TRUE(aggregate.continue_false);
    EXPECT_TRUE(aggregate.allowed);
    EXPECT_EQ(aggregate.reason, "stop now");
}

TEST(HookRuntime, UnsupportedOutputFieldsProduceDiagnostics) {
    auto parsed = acecode::parse_hook_process_output(
        ok_json_result(R"({
            "unknownTop": true,
            "hookSpecificOutput": {
                "unknownNested": "x"
            }
        })"),
        acecode::kCodexHookEventPreToolUse);

    acecode::HookAggregateOutcome aggregate;
    acecode::merge_hook_output(
        aggregate,
        parsed,
        acecode::kCodexHookEventPreToolUse,
        make_hook("h1", acecode::kCodexHookEventPreToolUse, "*"));

    ASSERT_EQ(aggregate.diagnostics.size(), 2u);
    EXPECT_EQ(aggregate.diagnostics[0].code, "HOOK_OUTPUT_UNSUPPORTED_FIELD");
    EXPECT_NE(aggregate.diagnostics[0].message.find("unknownTop"), std::string::npos);
    EXPECT_NE(aggregate.diagnostics[1].message.find("hookSpecificOutput.unknownNested"),
              std::string::npos);
}

TEST(HookManagerRuntime, DispatchSkipsPendingAndRunsTrustedHooks) {
    acecode::HookRegistrySnapshot snapshot;
    snapshot.feature_enabled = true;
    snapshot.hooks.push_back(make_hook(
        "pending", acecode::kCodexHookEventPreToolUse, "Bash",
        acecode::HookTrustStatus::PendingReview));
    snapshot.hooks.push_back(make_hook(
        "trusted", acecode::kCodexHookEventPreToolUse, "Bash",
        acecode::HookTrustStatus::Trusted));

    int invocations = 0;
    acecode::HookManager manager(std::move(snapshot),
        acecode::HookProcessRunner{},
        [&](const std::string& command,
            const std::string& stdin_text,
            int timeout_ms,
            const std::string& cwd) {
            (void)command;
            (void)stdin_text;
            (void)timeout_ms;
            (void)cwd;
            ++invocations;
            return ok_json_result(R"({
                "hookSpecificOutput": {
                    "permissionDecision": "deny",
                    "permissionDecisionReason": "blocked"
                }
            })");
        });

    acecode::HookDispatchRequest request;
    request.event_name = acecode::kCodexHookEventPreToolUse;
    request.matcher_value = "bash";
    request.cwd = ".";
    request.payload = nlohmann::json::object();

    auto outcome = manager.dispatch_codex(request);

    EXPECT_EQ(invocations, 1);
    EXPECT_EQ(outcome.matched_count, 2u);
    EXPECT_EQ(outcome.skipped_count, 1u);
    EXPECT_EQ(outcome.invoked_count, 1u);
    EXPECT_TRUE(outcome.denied);
    EXPECT_EQ(outcome.reason, "blocked");
}

TEST(HookManagerRuntime, DispatchSkipsDisabledAndRefreshesRegistrySnapshot) {
    acecode::HookRegistrySnapshot snapshot;
    snapshot.feature_enabled = true;
    snapshot.hooks.push_back(make_hook(
        "disabled", acecode::kCodexHookEventPreToolUse, "Bash",
        acecode::HookTrustStatus::Disabled));

    int invocations = 0;
    acecode::HookManager manager(std::move(snapshot),
        acecode::HookProcessRunner{},
        [&](const std::string&,
            const std::string&,
            int,
            const std::string&) {
            ++invocations;
            return ok_json_result("{}");
        });

    acecode::HookDispatchRequest request;
    request.event_name = acecode::kCodexHookEventPreToolUse;
    request.matcher_value = "bash";
    request.cwd = ".";
    request.payload = nlohmann::json::object();

    auto first = manager.dispatch_codex(request);
    EXPECT_EQ(first.matched_count, 1u);
    EXPECT_EQ(first.skipped_count, 1u);
    EXPECT_EQ(invocations, 0);

    acecode::HookRegistrySnapshot refreshed;
    refreshed.feature_enabled = true;
    refreshed.hooks.push_back(make_hook(
        "trusted", acecode::kCodexHookEventPreToolUse, "Bash",
        acecode::HookTrustStatus::Trusted));
    manager.refresh_registry(std::move(refreshed));

    auto second = manager.dispatch_codex(request);
    EXPECT_EQ(second.invoked_count, 1u);
    EXPECT_EQ(invocations, 1);
    EXPECT_EQ(manager.registry_snapshot().hooks[0].id, "trusted");
}
