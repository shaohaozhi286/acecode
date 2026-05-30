#include "browser_tools.hpp"

#include "session/session_manager.hpp"
#include "utils/logger.hpp"

#include <cctype>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>

namespace acecode::ace_browser_bridge {
namespace {

constexpr const char* kDefaultPort = "52007";

struct BrowserToolState {
    AceBrowserBridgeConfig config;
    std::shared_ptr<AceBrowserBridgeClient> client;
    std::mutex mu;
    std::set<std::string> prompted_sessions;
};

nlohmann::json object_schema(nlohmann::json properties,
                             nlohmann::json required = nlohmann::json::array()) {
    nlohmann::json schema;
    schema["type"] = "object";
    schema["properties"] = std::move(properties);
    schema["required"] = std::move(required);
    schema["additionalProperties"] = false;
    return schema;
}

nlohmann::json string_prop(const std::string& description) {
    return {{"type", "string"}, {"description", description}};
}

nlohmann::json envelope_json(const BridgeEnvelope& envelope) {
    nlohmann::json out;
    out["ok"] = envelope.ok;
    if (envelope.ok) {
        out["data"] = envelope.data.is_null() ? nlohmann::json::object() : envelope.data;
    } else {
        out["error"] = {
            {"code", envelope.error ? envelope.error->code : "bridge_error"},
            {"message", envelope.error ? envelope.error->message : "browser bridge command failed"},
        };
    }
    return out;
}

ToolResult envelope_result(const BridgeEnvelope& envelope) {
    return ToolResult{envelope_json(envelope).dump(2), envelope.ok};
}

bool parse_args(const std::string& arguments_json, nlohmann::json& args, std::string& error) {
    args = nlohmann::json::parse(arguments_json.empty() ? "{}" : arguments_json, nullptr, false);
    if (args.is_discarded() || !args.is_object()) {
        error = "arguments must be a JSON object";
        return false;
    }
    return true;
}

std::string optional_string(const nlohmann::json& args, const char* key) {
    if (args.contains(key) && args[key].is_string()) return args[key].get<std::string>();
    return {};
}

std::string sanitize_session_part(std::string value) {
    for (char& ch : value) {
        const unsigned char c = static_cast<unsigned char>(ch);
        if (!std::isalnum(c) && ch != '-' && ch != '_') ch = '-';
    }
    while (!value.empty() && value.front() == '-') value.erase(value.begin());
    while (!value.empty() && value.back() == '-') value.pop_back();
    if (value.size() > 48) value.resize(48);
    return value.empty() ? "default" : value;
}

std::string default_session_name(const nlohmann::json& args, const ToolContext& ctx) {
    std::string explicit_session = optional_string(args, "session");
    if (!explicit_session.empty()) return explicit_session;
    if (ctx.session_manager) {
        std::string session_id = ctx.session_manager->current_session_id();
        if (!session_id.empty()) return "acecode-" + sanitize_session_part(session_id);
    }
    return "acecode-default";
}

std::string cli_prompt_for_session(const std::string& session) {
#ifdef _WIN32
    const char* host = "ace-browser-host.exe";
    const char* host_example = "ace-browser-host.exe";
#else
    const char* host = "ace-browser-host";
    const char* host_example = "ace-browser-host";
#endif
    std::ostringstream oss;
    oss
        << "# ACE Browser Bridge\n\n"
        << "Use the local `" << host << "` CLI for browser work. The only ACECode browser tool is `browser_start`; "
        << "after it is called, do page operations with shell commands against `" << host << "`, not with `browser_*` tools.\n\n"
        << "Default session: `" << session << "`. Default daemon port: `" << kDefaultPort << "`. "
        << "CLI responses are always JSON envelopes: `{\"ok\":true,\"data\":...}` or "
        << "`{\"ok\":false,\"error\":{\"code\":\"...\",\"message\":\"...\"}}`.\n\n"
        << "Readiness:\n"
        << "```bash\n"
        << host_example << " ensure-ready --json\n"
        << host_example << " start --json\n"
        << host_example << " status --json\n"
        << "```\n"
        << "Use `ensure-ready` before page actions. It starts the host daemon, opens a normal browser wake page when the extension is not connected, and waits for a fresh bridge connection. "
        << "If it returns `ready:false`, inspect `ready_error`, `browser_launch_error`, `extension_connected`, `extension_stale`, and `version_compatible` before asking the user to fix the browser or extension.\n\n"
        << "Page selection and reading:\n"
        << "```bash\n"
        << host_example << " open --json --session " << session << " --url https://example.com\n"
        << host_example << " find-tab --json --session " << session << " --active\n"
        << host_example << " navigate --json --session " << session << " --operation reload\n"
        << host_example << " read-page --json --session " << session << " --mode summary\n"
        << host_example << " wait --json --session " << session << " --condition element_visible --target @e15 --timeout-ms 5000\n"
        << "```\n\n"
        << "`read-page` returns rich element metadata: value/options, disabled/loading/ARIA state, actionable, context, stable_selector, focused, viewport, and attachment refs (`@a`). "
        << "When `@e` refs or CSS are brittle, target by structured locator: `--locator '{\"role\":\"button\",\"name\":\"Save\",\"within\":{\"role\":\"row\",\"name\":\"BUG-123\"}}'`, or use `--role`/`--name`/`--near-text`/`--nth`.\n\n"
        << "Interaction:\n"
        << "```bash\n"
        << host_example << " click --json --session " << session << " --target @e15\n"
        << host_example << " click --json --session " << session << " --locator '{\"role\":\"button\",\"name\":\"Save\"}'\n"
        << host_example << " fill --json --session " << session << " --target @e1 --value \"text\"\n"
        << host_example << " type --json --session " << session << " --target @e2 --text \"abc\" --submit\n"
        << host_example << " hover --json --session " << session << " --target @e3\n"
        << host_example << " drag --json --session " << session << " --from @e4 --to @e5\n"
        << host_example << " scroll --json --session " << session << " --delta-y 700\n"
        << "```\n\n"
        << "Verification and batching:\n"
        << "```bash\n"
        << host_example << " assert --json --session " << session << " --condition text_equals --target @e7 --text \"Saved\" --timeout-ms 5000\n"
        << host_example << " assert --json --session " << session << " --condition request_completed --url \"/api/save\" --method POST --status-class 2xx --timeout-ms 10000\n"
        << host_example << " click --json --session " << session << " --target @e15 --args-json '{\"expect\":{\"condition\":\"text_equals\",\"target\":\"#status\",\"text\":\"Saved\",\"timeout_ms\":5000}}'\n"
        << "printf '%s' '{\"vars\":{\"status\":\"Done\"},\"steps\":[{\"action\":\"click\",\"args\":{\"locator\":{\"role\":\"button\",\"name\":\"Edit\"}}},{\"action\":\"fill\",\"args\":{\"role\":\"combobox\",\"name\":\"Status\",\"value\":\"${status}\"},\"retry\":{\"attempts\":3,\"delay_ms\":300}},{\"action\":\"click\",\"args\":{\"role\":\"button\",\"name\":\"Save\",\"expect\":{\"condition\":\"request_completed\",\"url\":\"/api/save\",\"method\":\"POST\",\"status_class\":\"2xx\",\"timeout_ms\":10000}}}],\"finally\":[{\"action\":\"read_page\",\"args\":{\"mode\":\"summary\"}}]}' | " << host_example << " batch --json --session " << session << "\n"
        << "```\n"
        << "`assert` and inline `expect` return `observed` and diagnostics on failure. Inline network `expect` is action-scoped by default; it ignores old matching requests. Use scoped DOM checks first (`text_equals`, `value_equals`, `element_absent`), then network checks (`network_idle`, `request_completed` after `network --cmd start`), and use element screenshots/attachments for vision only when DOM or network evidence is insufficient. "
        << "`batch` runs steps in one browser dispatch, stops on the first failing step by default, and supports vars, `${...}` interpolation, `set`, `when`, `retry`, and top-level `finally`. Pass batch JSON through stdin, `--steps-file`, or `--stdin-input-json`; do not leave `batch --json` waiting for interactive stdin.\n\n"
        << "Inspection and export:\n"
        << "```bash\n"
        << host_example << " evaluate --json --session " << session << " --code \"(() => document.title)()\"\n"
        << host_example << " network --json --session " << session << " --cmd start\n"
        << host_example << " network --json --session " << session << " --cmd list --filter api\n"
        << host_example << " screenshot --json --session " << session << " --output ./page.png\n"
        << host_example << " screenshot --json --session " << session << " --target @e4 --output ./element.png\n"
        << host_example << " screenshot --json --session " << session << " --attachment-ref @a1 --output ./attachment.png\n"
        << host_example << " save-pdf --json --session " << session << " --file-name page.pdf\n"
        << host_example << " list-tabs --json --session " << session << "\n"
        << host_example << " list-tabs --json --session " << session << " --all\n"
        << host_example << " close-session --json --session " << session << "\n"
        << "```\n\n"
        << "DevTools and raw CDP:\n"
        << "```bash\n"
        << host_example << " devtools --json --session " << session << " --cmd console-start\n"
        << host_example << " devtools --json --session " << session << " --cmd console-list --types error,warn\n"
        << host_example << " devtools --json --session " << session << " --cmd network-start\n"
        << host_example << " devtools --json --session " << session << " --cmd network-detail --request-id <id> --response-file ./response.txt\n"
        << host_example << " devtools --json --session " << session << " --cmd emulate --viewport 390x844x3,mobile,touch --network-conditions \"Slow 3G\"\n"
        << host_example << " devtools --json --session " << session << " --cmd performance-start --reload\n"
        << host_example << " devtools --json --session " << session << " --cmd performance-stop --output ./trace.json\n"
        << host_example << " devtools --json --session " << session << " --cmd heap-snapshot --output ./page.heapsnapshot\n"
        << host_example << " cdp --json --session " << session << " --method Runtime.evaluate --params '{\\\"expression\\\":\\\"document.title\\\",\\\"returnByValue\\\":true}'\n"
        << "```\n\n"
        << "Workflow rules:\n"
        << "- Prefer `read-page` first, then use returned `@e` refs, structured locators, or stable_selector for `click`, `fill`, `type`, `hover`, or `drag`; CSS selectors are fallback targets.\n"
        << "- For a sequence of related operations, prefer one `batch` with per-step `expect` / `assert`; for separate single commands, run the normal CLI actions directly. Page operation feedback and input guarding are automatic.\n"
        << "- When a click is expected to open a popup or new window (OAuth, a payment flow such as PayPal, or `target=_blank`), that window is a separate tab the session does not auto-follow. After the click, run `list-tabs --all`, locate the new tab whose `opener_tab_id` matches your session tab (or match by url/title), then `find-tab --tab-id <id>` to adopt and continue in it. Switch back with `find-tab --tab-id <original>` or `find-tab --active`.\n"
        << "- For elements inside an iframe (including cross-origin iframes like embedded payment fields), do not give up: `read-page` already aggregates iframe elements and tags them with `frame_id`/`frame_url`. Use the returned `@e` ref directly for `click`/`fill`/`type` and the bridge routes the action into the right frame. Structured locators and CSS selectors only resolve in the main frame, so prefer the `@e` ref for iframe targets.\n"
        << "- Omit `--new-tab` for normal in-page navigation; open another tab only when the task needs it.\n"
        << "- Use `fill` for replacing field text and `type` for keyboard-like input, shortcuts, or Enter submission.\n"
        << "- Use `evaluate` for compact DOM reads when `read-page` is too coarse.\n"
        << "- Use `devtools` for console, network details, emulation, performance traces, and heap snapshots; use `cdp` when a specific Chrome DevTools Protocol method is needed.\n"
        << "- Screenshots write files and return metadata; prefer `--target`/`--locator` element crops or `--attachment-ref` exports over full-page images for low-compute vision. If the active model cannot inspect images, pass the saved path to the `vision_analyze` tool via `image_path`; otherwise rely on `read-page`, `evaluate`, network data, and file metadata. Ask the user to visually inspect a saved file only when no vision-capable model is configured.\n";
    return oss.str();
}

bool should_emit_prompt(const std::shared_ptr<BrowserToolState>& state,
                        const std::string& session,
                        bool force_prompt) {
    std::lock_guard<std::mutex> lk(state->mu);
    if (force_prompt) {
        state->prompted_sessions.insert(session);
        return true;
    }
    return state->prompted_sessions.insert(session).second;
}

ToolImpl make_start_tool(const std::shared_ptr<BrowserToolState>& state) {
    ToolDef def;
    def.name = "browser_start";
    def.description =
        "Ensure ACE Browser Bridge host and browser are ready, then load the CLI usage prompt for this conversation.";
    def.parameters = object_schema({
        {"session", string_prop("Optional browser session name. Defaults to the current ACECode thread session.")},
        {"include_prompt", {{"type", "boolean"}, {"description", "Attach the CLI user prompt. Defaults to true."}}},
        {"force_prompt", {{"type", "boolean"}, {"description", "Attach the CLI prompt even if it was already loaded for this session."}}},
    });

    ToolImpl impl;
    impl.definition = std::move(def);
    impl.is_read_only = true;
    impl.execute = [state](const std::string& arguments_json, const ToolContext& ctx) {
        nlohmann::json args;
        std::string error;
        if (!parse_args(arguments_json, args, error)) {
            BridgeEnvelope envelope;
            envelope.ok = false;
            envelope.error = BridgeError{"invalid_arguments", error};
            return envelope_result(envelope);
        }

        const std::string session = default_session_name(args, ctx);
        LOG_INFO("[ace-browser-tool] browser_start session=" + session +
                 " include_prompt=" + std::string(args.value("include_prompt", true) ? "true" : "false") +
                 " force_prompt=" + std::string(args.value("force_prompt", false) ? "true" : "false"));
        BridgeEnvelope envelope = state->client->ensure_ready();
        nlohmann::json out = envelope_json(envelope);
        if (out["ok"].get<bool>()) {
            auto& data = out["data"];
            if (!data.is_object()) data = nlohmann::json::object();
            data["session"] = session;
            data["tool"] = "browser_start";
            data["cli"] = {
#ifdef _WIN32
                {"executable", "ace-browser-host.exe"},
#else
                {"executable", "ace-browser-host"},
#endif
                {"default_port", 52007},
                {"usage", "Use the local CLI commands described in the injected user prompt for all page actions."},
            };
            data["config"] = {
                {"pointer_speed", state->config.pointer_speed},
                {"default_mode", state->config.default_mode},
                {"operation_overlay_enabled", state->config.operation_overlay_enabled},
                {"os_pointer_enabled", state->config.os_pointer_enabled},
            };
        }

        ToolResult result{out.dump(2), envelope.ok};
        const bool include_prompt = args.value("include_prompt", true);
        const bool force_prompt = args.value("force_prompt", false);
        if (include_prompt && should_emit_prompt(state, session, force_prompt)) {
            result.post_user_prompt = cli_prompt_for_session(session);
            result.post_user_prompt_display_text = "[ACE Browser Bridge CLI prompt loaded]";
        }
        if (envelope.ok) {
            bool ready = out.contains("data") && out["data"].is_object() &&
                         out["data"].value("ready", false);
            LOG_INFO("[ace-browser-tool] browser_start finish session=" + session +
                     " ready=" + std::string(ready ? "true" : "false") +
                     " prompt_injected=" + std::string(result.post_user_prompt.has_value() ? "true" : "false"));
        } else {
            LOG_WARN("[ace-browser-tool] browser_start finish session=" + session +
                     " ok=false error_code=" +
                     (envelope.error ? envelope.error->code : std::string("bridge_error")));
        }
        return result;
    };
    return impl;
}

} // namespace

std::vector<std::string> ace_browser_core_tool_names() {
    return {"browser_start"};
}

std::vector<std::string> ace_browser_full_tool_names() {
    return {
        "browser_start",
        "browser_status",
        "browser_open",
        "browser_find_tab",
        "browser_navigate",
        "browser_read_page",
        "browser_wait",
        "browser_enable",
        "browser_close_session",
        "browser_click",
        "browser_fill",
        "browser_type",
        "browser_hover",
        "browser_drag",
        "browser_scroll",
        "browser_network",
        "browser_screenshot",
        "browser_save_pdf",
        "browser_trace",
        "browser_evaluate",
        "browser_upload",
        "browser_list_tabs",
    };
}

std::vector<std::string> ace_browser_group_names() {
    return {};
}

void register_ace_browser_bridge_tools(ToolExecutor& tools,
                                       const AceBrowserBridgeConfig& config,
                                       CliRunner runner) {
    if (!config.enabled) return;

    auto state = std::make_shared<BrowserToolState>();
    state->config = config;
    state->client = std::make_shared<AceBrowserBridgeClient>(config, std::move(runner));
    tools.register_tool(make_start_tool(state));
}

std::size_t unregister_ace_browser_bridge_tools(ToolExecutor& tools) {
    std::size_t removed = 0;
    for (const auto& name : ace_browser_full_tool_names()) {
        if (tools.unregister_tool(name)) ++removed;
    }
    return removed;
}

} // namespace acecode::ace_browser_bridge
