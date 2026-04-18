#include "config.hpp"

#include "../utils/logger.hpp"

#include <cstdlib>
#include <fstream>
#include <iostream>
#include <filesystem>

namespace fs = std::filesystem;

namespace acecode {

std::string expand_path(const std::string& raw) {
    if (raw.empty()) return raw;

    std::string result;
    result.reserve(raw.size());
    size_t i = 0;

    // ${VAR} substitution first so later ~ expansion sees final text.
    while (i < raw.size()) {
        if (raw[i] == '$' && i + 1 < raw.size() && raw[i + 1] == '{') {
            size_t end = raw.find('}', i + 2);
            if (end == std::string::npos) {
                result.push_back(raw[i++]);
                continue;
            }
            std::string var_name = raw.substr(i + 2, end - (i + 2));
            const char* val = std::getenv(var_name.c_str());
            if (val) {
                result.append(val);
            } else {
                result.append(raw, i, end + 1 - i); // leave ${VAR} untouched
            }
            i = end + 1;
        } else {
            result.push_back(raw[i++]);
        }
    }

    if (!result.empty() && result.front() == '~') {
#ifdef _WIN32
        const char* home = std::getenv("USERPROFILE");
#else
        const char* home = std::getenv("HOME");
#endif
        if (home && *home) {
            result = std::string(home) + result.substr(1);
        }
    }

    return result;
}

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
            if (j.contains("skills") && j["skills"].is_object()) {
                const auto& sj = j["skills"];
                if (sj.contains("disabled") && sj["disabled"].is_array()) {
                    for (const auto& v : sj["disabled"]) {
                        if (v.is_string()) cfg.skills.disabled.push_back(v.get<std::string>());
                    }
                }
                if (sj.contains("external_dirs") && sj["external_dirs"].is_array()) {
                    for (const auto& v : sj["external_dirs"]) {
                        if (v.is_string()) cfg.skills.external_dirs.push_back(v.get<std::string>());
                    }
                }
            }
            if (j.contains("mcp_servers") && j["mcp_servers"].is_object()) {
                for (auto it = j["mcp_servers"].begin(); it != j["mcp_servers"].end(); ++it) {
                    const std::string& server_name = it.key();
                    const auto& sj = it.value();
                    if (!sj.is_object()) {
                        LOG_WARN("[config] mcp_servers['" + server_name + "'] is not an object, skipping");
                        continue;
                    }

                    McpServerConfig mcfg;

                    // Determine transport. Missing field defaults to stdio so
                    // pre-existing configs keep working unchanged.
                    std::string transport_str = "stdio";
                    if (sj.contains("transport") && sj["transport"].is_string()) {
                        transport_str = sj["transport"].get<std::string>();
                    }
                    if (transport_str == "stdio") {
                        mcfg.transport = McpTransport::Stdio;
                    } else if (transport_str == "sse") {
                        mcfg.transport = McpTransport::Sse;
                    } else if (transport_str == "http") {
                        mcfg.transport = McpTransport::Http;
                    } else {
                        LOG_WARN("[config] mcp_servers['" + server_name +
                                 "'] has unknown transport '" + transport_str + "', skipping");
                        continue;
                    }

                    if (mcfg.transport == McpTransport::Stdio) {
                        if (!sj.contains("command") || !sj["command"].is_string() ||
                            sj["command"].get<std::string>().empty()) {
                            LOG_WARN("[config] mcp_servers['" + server_name +
                                     "'] stdio entry missing required 'command', skipping");
                            continue;
                        }
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
                    } else {
                        if (!sj.contains("url") || !sj["url"].is_string() ||
                            sj["url"].get<std::string>().empty()) {
                            LOG_WARN("[config] mcp_servers['" + server_name +
                                     "'] " + transport_str + " entry missing required 'url', skipping");
                            continue;
                        }
                        mcfg.url = sj["url"].get<std::string>();
                        if (sj.contains("sse_endpoint") && sj["sse_endpoint"].is_string()) {
                            mcfg.sse_endpoint = sj["sse_endpoint"].get<std::string>();
                        }
                        if (sj.contains("headers") && sj["headers"].is_object()) {
                            for (auto hit = sj["headers"].begin(); hit != sj["headers"].end(); ++hit) {
                                if (hit.value().is_string()) {
                                    mcfg.headers[hit.key()] = hit.value().get<std::string>();
                                }
                            }
                        }
                        if (sj.contains("auth_token") && sj["auth_token"].is_string()) {
                            mcfg.auth_token = sj["auth_token"].get<std::string>();
                        }
                        if (sj.contains("timeout_seconds") && sj["timeout_seconds"].is_number_integer()) {
                            int t = sj["timeout_seconds"].get<int>();
                            if (t > 0) mcfg.timeout_seconds = t;
                        }
                        if (sj.contains("validate_certificates") && sj["validate_certificates"].is_boolean()) {
                            mcfg.validate_certificates = sj["validate_certificates"].get<bool>();
                        }
                        if (sj.contains("ca_cert_path") && sj["ca_cert_path"].is_string()) {
                            mcfg.ca_cert_path = sj["ca_cert_path"].get<std::string>();
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

    if (!cfg.skills.disabled.empty() || !cfg.skills.external_dirs.empty()) {
        nlohmann::json sj = nlohmann::json::object();
        if (!cfg.skills.disabled.empty()) sj["disabled"] = cfg.skills.disabled;
        if (!cfg.skills.external_dirs.empty()) sj["external_dirs"] = cfg.skills.external_dirs;
        j["skills"] = sj;
    }

    if (!cfg.mcp_servers.empty()) {
        nlohmann::json mj = nlohmann::json::object();
        for (const auto& [name, srv] : cfg.mcp_servers) {
            nlohmann::json sj = nlohmann::json::object();
            if (srv.transport == McpTransport::Stdio) {
                // Omit the transport field for stdio so the resulting config
                // stays readable by older acecode builds.
                sj["command"] = srv.command;
                if (!srv.args.empty()) {
                    sj["args"] = srv.args;
                }
                if (!srv.env.empty()) {
                    nlohmann::json ej = nlohmann::json::object();
                    for (const auto& [k, v] : srv.env) ej[k] = v;
                    sj["env"] = ej;
                }
            } else {
                sj["transport"] = (srv.transport == McpTransport::Sse) ? "sse" : "http";
                sj["url"] = srv.url;
                if (srv.sse_endpoint != "/sse") {
                    sj["sse_endpoint"] = srv.sse_endpoint;
                }
                if (!srv.headers.empty()) {
                    nlohmann::json hj = nlohmann::json::object();
                    for (const auto& [k, v] : srv.headers) hj[k] = v;
                    sj["headers"] = hj;
                }
                if (!srv.auth_token.empty()) {
                    sj["auth_token"] = srv.auth_token;
                }
                if (srv.timeout_seconds != 30) {
                    sj["timeout_seconds"] = srv.timeout_seconds;
                }
                if (!srv.validate_certificates) {
                    sj["validate_certificates"] = false;
                }
                if (!srv.ca_cert_path.empty()) {
                    sj["ca_cert_path"] = srv.ca_cert_path;
                }
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
