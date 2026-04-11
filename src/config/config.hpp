#pragma once

#include <string>
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

struct AppConfig {
    std::string provider = "copilot"; // "copilot" or "openai"
    OpenAiConfig openai;
    CopilotConfig copilot;
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
