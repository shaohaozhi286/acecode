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

struct McpServerConfig {
    std::string command;                         // required: executable to launch
    std::vector<std::string> args;               // optional: CLI arguments
    std::map<std::string, std::string> env;      // optional: environment variables
};

struct AppConfig {
    std::string provider = "copilot"; // "copilot" or "openai"
    OpenAiConfig openai;
    CopilotConfig copilot;
    int context_window = 128000; // model context window size in tokens
    int max_sessions = 50;       // max saved sessions per project
    std::map<std::string, McpServerConfig> mcp_servers; // MCP stdio servers (optional)
};

// Load config from ~/.acecode/config.json, with env var overrides.
// Creates default config if missing.
AppConfig load_config();

// Save config to ~/.acecode/config.json.
// Creates directory if missing, overwrites existing file.
void save_config(const AppConfig& cfg);

// Get the path to ~/.acecode/ directory
std::string get_acecode_dir();

} // namespace acecode
