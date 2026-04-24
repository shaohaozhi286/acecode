#pragma once

#include "saved_models.hpp"

#include <map>
#include <optional>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>

namespace acecode {

struct OpenAiConfig {
    std::string base_url = "http://localhost:1234/v1";
    std::string api_key;
    std::string model = "local-model";
    // Optional provider id from the bundled models.dev registry (e.g. "anthropic",
    // "openrouter"). Lets resolve_model_context_window() and other catalog-aware
    // call sites pick the correct provider entry even when base_url is a proxy.
    std::optional<std::string> models_dev_provider_id;
};

struct CopilotConfig {
    std::string model = "gpt-4o";
};

enum class McpTransport {
    Stdio = 0, // launch a child process and talk over its stdio pipes
    Sse,       // HTTP + text/event-stream via mcp::sse_client
    Http,      // MCP Streamable HTTP (currently routed through sse_client)
};

struct McpServerConfig {
    McpTransport transport = McpTransport::Stdio;

    // Stdio-only fields.
    std::string command;                         // required for stdio: executable to launch
    std::vector<std::string> args;               // optional: CLI arguments
    std::map<std::string, std::string> env;      // optional: environment variables

    // SSE / HTTP fields.
    std::string url;                             // required for sse/http: scheme://host[:port]
    std::string sse_endpoint = "/sse";           // path portion of SSE/HTTP endpoint
    std::map<std::string, std::string> headers;  // optional extra request headers
    std::string auth_token;                      // optional bearer token (never logged)
    int timeout_seconds = 30;
    bool validate_certificates = true;
    std::string ca_cert_path;                    // optional CA bundle for self-signed TLS
};

struct SkillsConfig {
    std::vector<std::string> disabled;       // skill names to hide even if present on disk
    std::vector<std::string> external_dirs;  // extra directories to scan (supports ~ and ${ENV})
};

struct MemoryConfig {
    bool enabled = true;
    // Hard cap on MEMORY.md size for system-prompt injection. Oversized indexes
    // are truncated in-memory with a marker; the on-disk file is untouched.
    std::size_t max_index_bytes = 32 * 1024;
};

struct ProjectInstructionsConfig {
    bool enabled = true;
    int max_depth = 8;                         // max dirs walked from cwd towards HOME
    std::size_t max_bytes = 256 * 1024;        // per-file cap
    std::size_t max_total_bytes = 1024 * 1024; // aggregate cap for merged text
    // Priority order. Each directory contributes at most the first filename that
    // exists. ACECODE.md is native; AGENT.md and CLAUDE.md are compat.
    std::vector<std::string> filenames = {"ACECODE.md", "AGENT.md", "CLAUDE.md"};
    // Per-filename gates. Setting either to false removes that name from the
    // effective search list at runtime (overriding its presence in filenames).
    bool read_agent_md = true;
    bool read_claude_md = true;
};

struct DaemonConfig {
    bool auto_start_on_double_click = false;
    std::string service_name = "ACECodeDaemon";
    int heartbeat_interval_ms = 2000;
    int heartbeat_timeout_ms = 15000;
};

struct WebConfig {
    bool enabled = true;
    std::string bind = "127.0.0.1";
    int port = 26419;
    // Empty = serve embedded assets; non-empty = serve from this filesystem path
    // (development mode for the front-end change).
    std::string static_dir;
};

struct ModelsDevConfig {
    bool allow_network = false;                          // permit any HTTP request to models.dev
    std::optional<std::string> user_override_path;       // local api.json that beats the bundled snapshot
    bool refresh_on_command_only = true;                 // suppress all startup-time network refresh
};

struct InputHistoryConfig {
    bool enabled = true;        // disable to fall back to pure in-memory history
    int max_entries = 10;       // hard cap on persisted entries per working directory
};

struct AppConfig {
    std::string provider = "copilot"; // "copilot" or "openai"
    OpenAiConfig openai;
    CopilotConfig copilot;
    int context_window = 128000; // model context window size in tokens
    int max_sessions = 50;       // max saved sessions per project
    std::map<std::string, McpServerConfig> mcp_servers; // MCP stdio servers (optional)
    SkillsConfig skills;                         // skill system configuration (optional)
    MemoryConfig memory;                         // persistent user memory settings
    ProjectInstructionsConfig project_instructions; // ACECODE.md / AGENT.md / CLAUDE.md loader
    DaemonConfig daemon;                         // daemon process supervision settings
    WebConfig web;                               // HTTP/WebSocket server settings
    ModelsDevConfig models_dev;                  // bundled models.dev registry behaviour
    InputHistoryConfig input_history;            // per-cwd persistent ↑/↓ history

    // --- model profiles (openspec/changes/model-profiles) ---
    // 用户维护的命名模型列表。为空时 legacy 字段作为兜底 entry "(legacy)"。
    std::vector<ModelProfile> saved_models;
    // 指向 saved_models 中一个 entry 的 name;空字符串 = 未设定 = 走 legacy 兜底。
    std::string default_model_name;
};

// Expand ~ and ${ENV} style variables in a path string. Returns the expanded
// form; missing env vars are left as-is (per hermes convention).
std::string expand_path(const std::string& raw);

// Collect project-level directories from cwd up to (but not including) the
// user's home directory. Returned deepest-first so cwd-level skills take
// precedence over ancestor-level skills when scanned in order. HOME itself is
// excluded because the user-global skills root (`~/.acecode/skills`) is
// registered separately.
std::vector<std::string> get_project_dirs_up_to_home(const std::string& cwd);

// Load config from ~/.acecode/config.json, with env var overrides.
// Creates default config if missing.
AppConfig load_config();

// Save config to ~/.acecode/config.json.
// Creates directory if missing, overwrites existing file.
void save_config(const AppConfig& cfg);

// Get the path to ~/.acecode/ directory
std::string get_acecode_dir();

// Get the path to ~/.acecode/run/ (creates it if missing on first call site —
// callers are responsible for filesystem::create_directories when needed).
std::string get_run_dir();

// Get the path to ~/.acecode/logs/ (callers handle create_directories).
std::string get_logs_dir();

// Validate runtime-affecting config values. Returns an empty vector on success;
// otherwise each entry is a human-readable error message. Daemon mode callers
// should abort on non-empty result.
std::vector<std::string> validate_config(const AppConfig& cfg);

} // namespace acecode
