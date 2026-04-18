#pragma once

#include <map>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>

namespace acecode {

struct OpenAiConfig {
    std::string base_url = "http://localhost:1234/v1";
    std::string api_key;
    std::string model = "local-model";
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

struct AppConfig {
    std::string provider = "copilot"; // "copilot" or "openai"
    OpenAiConfig openai;
    CopilotConfig copilot;
    int context_window = 128000; // model context window size in tokens
    int max_sessions = 50;       // max saved sessions per project
    std::map<std::string, McpServerConfig> mcp_servers; // MCP stdio servers (optional)
    SkillsConfig skills;                         // skill system configuration (optional)
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

} // namespace acecode
