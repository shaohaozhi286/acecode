#pragma once

#include "hook_registry.hpp"
#include "hook_runner.hpp"

#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace acecode {

constexpr const char* kCodexHookEventSessionStart = "SessionStart";
constexpr const char* kCodexHookEventUserPromptSubmit = "UserPromptSubmit";
constexpr const char* kCodexHookEventPreToolUse = "PreToolUse";
constexpr const char* kCodexHookEventPermissionRequest = "PermissionRequest";
constexpr const char* kCodexHookEventPostToolUse = "PostToolUse";
constexpr const char* kCodexHookEventPreCompact = "PreCompact";
constexpr const char* kCodexHookEventPostCompact = "PostCompact";
constexpr const char* kCodexHookEventStop = "Stop";

struct HookCommonPayloadFields {
    std::string session_id;
    std::string transcript_path;
    std::string cwd;
    std::string hook_event_name;
    std::string model;
    std::string permission_mode;
    std::string turn_id;
};

struct HookDispatchRequest {
    std::string event_name;
    std::string matcher_value;
    std::string cwd;
    nlohmann::json payload = nlohmann::json::object();
};

struct HookParsedOutput {
    bool valid_json = false;
    nlohmann::json json = nlohmann::json::object();
    std::string plain_stdout;
    std::string stderr_text;
    int exit_code = 0;
    bool timed_out = false;
    bool failed = false;
    std::string failure_message;
};

struct HookAggregateOutcome {
    std::size_t matched_count = 0;
    std::size_t invoked_count = 0;
    std::size_t skipped_count = 0;
    bool blocked = false;
    bool denied = false;
    bool allowed = false;
    bool no_decision = true;
    bool continue_false = false;
    std::string reason;
    std::optional<nlohmann::json> updated_input;
    std::optional<std::string> replacement_output;
    std::vector<std::string> additional_context;
    std::vector<std::string> system_messages;
    std::vector<HookDiagnostic> diagnostics;
};

std::string canonical_hook_match_value(const std::string& value);
bool hook_matcher_matches(const NormalizedHook& hook,
                          const std::string& event_name,
                          const std::string& matcher_value,
                          std::vector<HookDiagnostic>* diagnostics = nullptr);

nlohmann::json build_hook_common_payload(const HookCommonPayloadFields& fields);
nlohmann::json build_session_start_hook_payload(const HookCommonPayloadFields& fields,
                                                const std::string& source);
nlohmann::json build_user_prompt_submit_hook_payload(const HookCommonPayloadFields& fields,
                                                     const std::string& prompt);
nlohmann::json build_tool_hook_payload(const HookCommonPayloadFields& fields,
                                       const std::string& tool_name,
                                       const nlohmann::json& tool_input,
                                       const nlohmann::json& tool_response = nullptr);
nlohmann::json build_compact_hook_payload(const HookCommonPayloadFields& fields,
                                          const std::string& trigger);
nlohmann::json build_stop_hook_payload(const HookCommonPayloadFields& fields,
                                       bool stop_hook_active,
                                       const std::string& last_assistant_message);

HookParsedOutput parse_hook_process_output(const HookProcessResult& result,
                                           const std::string& event_name);
void merge_hook_output(HookAggregateOutcome& aggregate,
                       const HookParsedOutput& output,
                       const std::string& event_name,
                       const NormalizedHook& hook);

} // namespace acecode
