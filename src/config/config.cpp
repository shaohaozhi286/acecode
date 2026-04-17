#include "config.hpp"

#include "../utils/logger.hpp"

#include <cstdlib>
#include <fstream>
#include <iostream>
#include <filesystem>

namespace fs = std::filesystem;

namespace acecode {

std::string get_acecode_dir() {
    std::string home;
#ifdef _WIN32
    const char* userprofile = std::getenv("USERPROFILE");
    if (userprofile) {
        home = userprofile;
    }
#else
    const char* home_env = std::getenv("HOME");
    if (home_env) {
        home = home_env;
    }
#endif
    if (home.empty()) {
        home = ".";
    }
    return (fs::path(home) / ".acecode").string();
}

static void write_default_config(const std::string& config_path) {
    nlohmann::json j;
    j["provider"] = "copilot";
    j["openai"]["base_url"] = "http://localhost:1234/v1";
    j["openai"]["api_key"] = "";
    j["openai"]["model"] = "local-model";
    j["copilot"]["model"] = "gpt-4o";

    std::ofstream ofs(config_path);
    if (ofs.is_open()) {
        ofs << j.dump(2) << std::endl;
    }
}

AppConfig load_config() {
    AppConfig cfg;

    std::string acecode_dir = get_acecode_dir();
    std::string config_path = (fs::path(acecode_dir) / "config.json").string();

    // Create directory and default config if missing
    if (!fs::exists(acecode_dir)) {
        fs::create_directories(acecode_dir);
    }
    if (!fs::exists(config_path)) {
        write_default_config(config_path);
    }

    // Read config file
    std::ifstream ifs(config_path);
    if (ifs.is_open()) {
        try {
            nlohmann::json j = nlohmann::json::parse(ifs);

            if (j.contains("provider") && j["provider"].is_string()) {
                cfg.provider = j["provider"].get<std::string>();
            }
            if (j.contains("openai") && j["openai"].is_object()) {
                auto& oj = j["openai"];
                if (oj.contains("base_url") && oj["base_url"].is_string())
                    cfg.openai.base_url = oj["base_url"].get<std::string>();
                if (oj.contains("api_key") && oj["api_key"].is_string())
                    cfg.openai.api_key = oj["api_key"].get<std::string>();
                if (oj.contains("model") && oj["model"].is_string())
                    cfg.openai.model = oj["model"].get<std::string>();
            }
            if (j.contains("copilot") && j["copilot"].is_object()) {
                auto& cj = j["copilot"];
                if (cj.contains("model") && cj["model"].is_string())
                    cfg.copilot.model = cj["model"].get<std::string>();
            }
            if (j.contains("context_window") && j["context_window"].is_number_integer()) {
                cfg.context_window = j["context_window"].get<int>();
            }
            if (j.contains("max_sessions") && j["max_sessions"].is_number_integer()) {
                cfg.max_sessions = j["max_sessions"].get<int>();
            }
            if (j.contains("mcp_servers") && j["mcp_servers"].is_object()) {
                for (auto it = j["mcp_servers"].begin(); it != j["mcp_servers"].end(); ++it) {
                    const std::string& server_name = it.key();
                    const auto& sj = it.value();
                    if (!sj.is_object()) {
                        LOG_WARN("[config] mcp_servers['" + server_name + "'] is not an object, skipping");
                        continue;
                    }
                    if (!sj.contains("command") || !sj["command"].is_string() ||
                        sj["command"].get<std::string>().empty()) {
                        LOG_WARN("[config] mcp_servers['" + server_name + "'] missing required 'command', skipping");
                        continue;
                    }
                    McpServerConfig mcfg;
                    mcfg.command = sj["command"].get<std::string>();
                    if (sj.contains("args") && sj["args"].is_array()) {
                        for (const auto& a : sj["args"]) {
                            if (a.is_string()) mcfg.args.push_back(a.get<std::string>());
                        }
                    }
                    if (sj.contains("env") && sj["env"].is_object()) {
                        for (auto eit = sj["env"].begin(); eit != sj["env"].end(); ++eit) {
                            if (eit.value().is_string()) {
                                mcfg.env[eit.key()] = eit.value().get<std::string>();
                            }
                        }
                    }
                    cfg.mcp_servers[server_name] = std::move(mcfg);
                }
            }
        } catch (const nlohmann::json::parse_error& e) {
            std::cerr << "[config] Warning: Failed to parse config.json: " << e.what() << std::endl;
        }
    }

    // Environment variable overrides
    if (const char* env = std::getenv("ACECODE_PROVIDER")) {
        cfg.provider = env;
    }
    if (const char* env = std::getenv("ACECODE_OPENAI_BASE_URL")) {
        cfg.openai.base_url = env;
    }
    if (const char* env = std::getenv("ACECODE_OPENAI_API_KEY")) {
        cfg.openai.api_key = env;
    }
    if (const char* env = std::getenv("ACECODE_MODEL")) {
        if (cfg.provider == "openai") {
            cfg.openai.model = env;
        } else {
            cfg.copilot.model = env;
        }
    }

    return cfg;
}

void save_config(const AppConfig& cfg) {
    std::string acecode_dir = get_acecode_dir();
    std::string config_path = (fs::path(acecode_dir) / "config.json").string();

    if (!fs::exists(acecode_dir)) {
        fs::create_directories(acecode_dir);
    }

    nlohmann::json j;
    j["provider"] = cfg.provider;
    j["openai"]["base_url"] = cfg.openai.base_url;
    j["openai"]["api_key"] = cfg.openai.api_key;
    j["openai"]["model"] = cfg.openai.model;
    j["copilot"]["model"] = cfg.copilot.model;
    j["context_window"] = cfg.context_window;
    j["max_sessions"] = cfg.max_sessions;

    if (!cfg.mcp_servers.empty()) {
        nlohmann::json mj = nlohmann::json::object();
        for (const auto& [name, srv] : cfg.mcp_servers) {
            nlohmann::json sj;
            sj["command"] = srv.command;
            if (!srv.args.empty()) {
                sj["args"] = srv.args;
            }
            if (!srv.env.empty()) {
                nlohmann::json ej = nlohmann::json::object();
                for (const auto& [k, v] : srv.env) ej[k] = v;
                sj["env"] = ej;
            }
            mj[name] = sj;
        }
        j["mcp_servers"] = mj;
    }

    std::ofstream ofs(config_path);
    if (ofs.is_open()) {
        ofs << j.dump(2) << std::endl;
    }
}

} // namespace acecode
