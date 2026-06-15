#include "hook_runtime.hpp"

#include "../utils/encoding.hpp"

#include <algorithm>
#include <cctype>
#include <regex>
#include <sstream>

namespace acecode {
namespace {

std::string trim_ascii(std::string value) {
    auto is_space = [](unsigned char c) { return std::isspace(c) != 0; };
    while (!value.empty() && is_space(static_cast<unsigned char>(value.front()))) {
        value.erase(value.begin());
    }
    while (!value.empty() && is_space(static_cast<unsigned char>(value.back()))) {
        value.pop_back();
    }
    return value;
}

std::string lower_ascii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

bool is_match_all(const std::string& matcher) {
    std::string m = trim_ascii(matcher);
    return m.empty() || m == "*";
}

bool alias_matches(const std::string& matcher, const std::string& target) {
    const std::string m = trim_ascii(matcher);
    const std::string t = trim_ascii(target);
    if (m == "Bash") return t == "bash";
    if (m == "Edit") return t == "file_edit";
    if (m == "Write") return t == "file_write";
    if (m == "apply_patch") return t == "file_edit" || t == "file_write";
    return false;
}

void add_diag(std::vector<HookDiagnostic>* diagnostics,
              HookDiagnosticSeverity severity,
              std::string code,
              std::string message,
              const NormalizedHook& hook) {
    if (!diagnostics) return;
    HookDiagnostic d;
    d.severity = severity;
    d.code = std::move(code);
    d.message = std::move(message);
    d.source_id = hook.source_id;
    d.hook_id = hook.id;
    d.event_name = hook.event_name;
    diagnostics->push_back(std::move(d));
}

std::string json_string_value(const nlohmann::json& obj,
                              const std::string& key) {
    if (!obj.is_object() || !obj.contains(key)) return {};
    const auto& v = obj[key];
    if (v.is_string()) return v.get<std::string>();
    if (v.is_object() || v.is_array()) return v.dump();
    if (v.is_boolean()) return v.get<bool>() ? "true" : "false";
    if (v.is_number()) return v.dump();
    return {};
}

void append_context_values(std::vector<std::string>& out, const nlohmann::json& value) {
    if (value.is_string()) {
        std::string s = value.get<std::string>();
        if (!s.empty()) out.push_back(std::move(s));
        return;
    }
    if (value.is_array()) {
        for (const auto& item : value) append_context_values(out, item);
        return;
    }
    if (value.is_object()) out.push_back(value.dump());
}

void append_system_message(std::vector<std::string>& out, const std::string& msg) {
    if (!msg.empty()) out.push_back(msg);
}

std::string first_non_empty(std::initializer_list<std::string> values) {
    for (const auto& value : values) {
        if (!value.empty()) return value;
    }
    return {};
}

std::string reason_from_json(const nlohmann::json& root,
                             const nlohmann::json& specific) {
    return first_non_empty({
        json_string_value(specific, "permissionDecisionReason"),
        json_string_value(specific, "reason"),
        json_string_value(root, "reason"),
        json_string_value(root, "message"),
        json_string_value(root, "systemMessage"),
    });
}

void apply_decision(HookAggregateOutcome& aggregate,
                    const std::string& decision_raw,
                    const std::string& reason,
                    const std::string& event_name) {
    const std::string decision = lower_ascii(decision_raw);
    if (decision.empty()) return;

    if (decision == "block" || decision == "deny" || decision == "denied") {
        aggregate.blocked = true;
        aggregate.denied = true;
        aggregate.allowed = false;
        aggregate.no_decision = false;
        if (aggregate.reason.empty()) aggregate.reason = reason;
        return;
    }
    if (decision == "allow" || decision == "allowed" || decision == "approve") {
        if (!aggregate.denied && !aggregate.blocked) {
            aggregate.allowed = true;
            aggregate.no_decision = false;
            if (aggregate.reason.empty()) aggregate.reason = reason;
        }
        return;
    }
    if (decision == "ask" || decision == "none" || decision == "no_decision") {
        (void)event_name;
        return;
    }
}

bool plain_stdout_is_context_event(const std::string& event_name) {
    return event_name == kCodexHookEventSessionStart ||
           event_name == kCodexHookEventUserPromptSubmit;
}

bool contains_string(const std::vector<std::string>& values, const std::string& value) {
    return std::find(values.begin(), values.end(), value) != values.end();
}

void append_unsupported_output_field_diagnostics(HookAggregateOutcome& aggregate,
                                                 const nlohmann::json& root,
                                                 const nlohmann::json& specific,
                                                 const std::string& event_name,
                                                 const NormalizedHook& hook) {
    const std::vector<std::string> root_allowed = {
        "additionalContext",
        "continue",
        "decision",
        "feedback",
        "hookSpecificOutput",
        "message",
        "permissionDecision",
        "permissionDecisionReason",
        "reason",
        "stopReason",
        "systemMessage",
        "updatedInput",
    };
    const std::vector<std::string> specific_allowed = {
        "additionalContext",
        "feedback",
        "permissionDecision",
        "permissionDecisionReason",
        "reason",
        "updatedInput",
    };

    auto add = [&](const std::string& field) {
        HookDiagnostic d;
        d.severity = HookDiagnosticSeverity::Warning;
        d.code = "HOOK_OUTPUT_UNSUPPORTED_FIELD";
        d.message = "unsupported output field for " + event_name + ": " + field;
        d.source_id = hook.source_id;
        d.hook_id = hook.id;
        d.event_name = event_name;
        aggregate.diagnostics.push_back(std::move(d));
    };

    if (root.is_object()) {
        for (auto it = root.begin(); it != root.end(); ++it) {
            if (!contains_string(root_allowed, it.key())) add(it.key());
        }
    }
    if (specific.is_object()) {
        for (auto it = specific.begin(); it != specific.end(); ++it) {
            if (!contains_string(specific_allowed, it.key())) {
                add("hookSpecificOutput." + it.key());
            }
        }
    }
}

} // namespace

std::string canonical_hook_match_value(const std::string& value) {
    const std::string v = trim_ascii(value);
    if (v == "Bash") return "bash";
    if (v == "Edit") return "file_edit";
    if (v == "Write") return "file_write";
    if (v == "apply_patch") return "file_edit";
    return v;
}

bool hook_matcher_matches(const NormalizedHook& hook,
                          const std::string& event_name,
                          const std::string& matcher_value,
                          std::vector<HookDiagnostic>* diagnostics) {
    if (hook.event_name != event_name) return false;
    if (event_name == kCodexHookEventUserPromptSubmit ||
        event_name == kCodexHookEventStop) {
        return true;
    }

    const std::string matcher = trim_ascii(hook.matcher);
    if (is_match_all(matcher)) return true;
    const std::string target = canonical_hook_match_value(matcher_value);
    if (alias_matches(matcher, target)) return true;

    try {
        std::regex re(matcher);
        return std::regex_search(matcher_value, re) ||
               (target != matcher_value && std::regex_search(target, re));
    } catch (const std::regex_error& e) {
        add_diag(diagnostics,
                 HookDiagnosticSeverity::Warning,
                 "HOOK_MATCHER_INVALID_REGEX",
                 std::string("invalid hook matcher regex: ") + e.what(),
                 hook);
        return false;
    }
}

nlohmann::json build_hook_common_payload(const HookCommonPayloadFields& fields) {
    nlohmann::json payload = nlohmann::json::object();
    payload["session_id"] = fields.session_id;
    payload["transcript_path"] = fields.transcript_path;
    payload["cwd"] = fields.cwd;
    payload["hook_event_name"] = fields.hook_event_name;
    payload["model"] = fields.model;
    if (!fields.permission_mode.empty()) payload["permission_mode"] = fields.permission_mode;
    if (!fields.turn_id.empty()) payload["turn_id"] = fields.turn_id;
    return payload;
}

nlohmann::json build_session_start_hook_payload(const HookCommonPayloadFields& fields,
                                                const std::string& source) {
    auto payload = build_hook_common_payload(fields);
    payload["source"] = source;
    return payload;
}

nlohmann::json build_user_prompt_submit_hook_payload(const HookCommonPayloadFields& fields,
                                                     const std::string& prompt) {
    auto payload = build_hook_common_payload(fields);
    payload["prompt"] = prompt;
    return payload;
}

nlohmann::json build_tool_hook_payload(const HookCommonPayloadFields& fields,
                                       const std::string& tool_name,
                                       const nlohmann::json& tool_input,
                                       const nlohmann::json& tool_response) {
    auto payload = build_hook_common_payload(fields);
    payload["tool_name"] = tool_name;
    payload["tool_input"] = tool_input.is_discarded() ? nlohmann::json::object() : tool_input;
    if (!tool_response.is_null()) payload["tool_response"] = tool_response;
    return payload;
}

nlohmann::json build_compact_hook_payload(const HookCommonPayloadFields& fields,
                                          const std::string& trigger) {
    auto payload = build_hook_common_payload(fields);
    payload["trigger"] = trigger;
    return payload;
}

nlohmann::json build_stop_hook_payload(const HookCommonPayloadFields& fields,
                                       bool stop_hook_active,
                                       const std::string& last_assistant_message) {
    auto payload = build_hook_common_payload(fields);
    payload["stop_hook_active"] = stop_hook_active;
    payload["last_assistant_message"] = last_assistant_message;
    return payload;
}

HookParsedOutput parse_hook_process_output(const HookProcessResult& result,
                                           const std::string& event_name) {
    (void)event_name;
    HookParsedOutput out;
    out.exit_code = result.exit_code;
    out.timed_out = result.timed_out;
    out.stderr_text = result.stderr_text;

    if (result.timed_out) {
        out.failed = true;
        out.failure_message = "hook timed out";
    } else if (!result.started) {
        out.failed = true;
        out.failure_message = result.error.empty() ? "hook failed to start" : result.error;
    } else if (result.exit_code != 0 && result.exit_code != 2) {
        out.failed = true;
        out.failure_message = result.error.empty()
            ? first_non_empty({trim_ascii(result.stderr_text),
                               trim_ascii(result.stdout_text),
                               "hook exited with code " + std::to_string(result.exit_code)})
            : result.error;
    }

    std::string stdout_text = trim_ascii(result.stdout_text);
    if (stdout_text.empty() && !result.output.empty() && result.stdout_text.empty() &&
        result.stderr_text.empty()) {
        stdout_text = trim_ascii(result.output);
    }
    if (!stdout_text.empty()) {
        try {
            auto parsed = nlohmann::json::parse(stdout_text);
            if (parsed.is_object()) {
                out.valid_json = true;
                out.json = std::move(parsed);
            } else {
                out.plain_stdout = stdout_text;
            }
        } catch (...) {
            out.plain_stdout = stdout_text;
        }
    }
    return out;
}

void merge_hook_output(HookAggregateOutcome& aggregate,
                       const HookParsedOutput& output,
                       const std::string& event_name,
                       const NormalizedHook& hook) {
    if (output.timed_out || output.failed) {
        HookDiagnostic d;
        d.severity = HookDiagnosticSeverity::Warning;
        d.code = output.timed_out ? "HOOK_TIMEOUT" : "HOOK_FAILED";
        d.message = output.failure_message;
        d.source_id = hook.source_id;
        d.hook_id = hook.id;
        d.event_name = event_name;
        aggregate.diagnostics.push_back(std::move(d));
        return;
    }

    if (output.exit_code == 2) {
        const std::string reason = first_non_empty({
            trim_ascii(output.stderr_text),
            trim_ascii(output.plain_stdout),
            "hook blocked " + event_name,
        });
        aggregate.blocked = true;
        aggregate.denied = true;
        aggregate.allowed = false;
        aggregate.no_decision = false;
        if (aggregate.reason.empty()) aggregate.reason = reason;
        return;
    }

    if (!output.valid_json) {
        if (!output.plain_stdout.empty() && plain_stdout_is_context_event(event_name)) {
            aggregate.additional_context.push_back(output.plain_stdout);
        }
        return;
    }

    const auto& root = output.json;
    const nlohmann::json specific =
        root.contains("hookSpecificOutput") && root["hookSpecificOutput"].is_object()
            ? root["hookSpecificOutput"]
            : nlohmann::json::object();
    append_unsupported_output_field_diagnostics(
        aggregate, root, specific, event_name, hook);

    if (root.contains("continue") && root["continue"].is_boolean() &&
        !root["continue"].get<bool>()) {
        aggregate.continue_false = true;
        aggregate.no_decision = false;
        const std::string reason = reason_from_json(root, specific);
        if (aggregate.reason.empty()) aggregate.reason = reason;
    }

    append_system_message(aggregate.system_messages, json_string_value(root, "systemMessage"));
    if (root.contains("additionalContext")) {
        append_context_values(aggregate.additional_context, root["additionalContext"]);
    }
    if (specific.contains("additionalContext")) {
        append_context_values(aggregate.additional_context, specific["additionalContext"]);
    }
    if (specific.contains("updatedInput")) {
        aggregate.updated_input = specific["updatedInput"];
    } else if (root.contains("updatedInput")) {
        aggregate.updated_input = root["updatedInput"];
    }

    const std::string reason = reason_from_json(root, specific);
    apply_decision(aggregate, json_string_value(root, "decision"), reason, event_name);
    apply_decision(aggregate, json_string_value(specific, "permissionDecision"), reason, event_name);
    apply_decision(aggregate, json_string_value(root, "permissionDecision"), reason, event_name);

    const std::string feedback = first_non_empty({
        json_string_value(specific, "feedback"),
        json_string_value(root, "feedback"),
        json_string_value(root, "message"),
        reason,
    });
    if (event_name == kCodexHookEventPostToolUse &&
        (aggregate.continue_false || aggregate.blocked) &&
        !feedback.empty()) {
        aggregate.replacement_output = feedback;
    }
}

} // namespace acecode
