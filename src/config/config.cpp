#include "config.hpp"

#include "../utils/constants.hpp"
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

std::vector<std::string> get_project_dirs_up_to_home(const std::string& cwd) {
    std::vector<std::string> dirs;
    if (cwd.empty()) return dirs;

    std::error_code ec;
    fs::path abs = fs::weakly_canonical(fs::path(cwd), ec);
    if (ec || abs.empty()) abs = fs::path(cwd);

    fs::path home_path;
#ifdef _WIN32
    const char* home_env = std::getenv("USERPROFILE");
#else
    const char* home_env = std::getenv("HOME");
#endif
    if (home_env && *home_env) {
        std::error_code hec;
        home_path = fs::weakly_canonical(fs::path(home_env), hec);
        if (hec) home_path = fs::path(home_env);
    }

    // Walk up from cwd; stop at/above HOME (the user-global root is added
    // separately) or once we hit a filesystem root. Deepest first.
    fs::path cur = abs;
    while (true) {
        if (!home_path.empty() && cur == home_path) break;
        dirs.push_back(cur.string());
        fs::path parent = cur.parent_path();
        if (parent == cur) break;
        cur = parent;
    }
    return dirs;
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

std::string get_run_dir() {
    return (fs::path(get_acecode_dir()) / constants::SUBDIR_RUN).string();
}

std::string get_logs_dir() {
    return (fs::path(get_acecode_dir()) / constants::SUBDIR_LOGS).string();
}

std::vector<std::string> validate_config(const AppConfig& cfg) {
    std::vector<std::string> errors;
    if (cfg.web.port < 1 || cfg.web.port > 65535) {
        errors.push_back("web.port out of range (1-65535): " + std::to_string(cfg.web.port));
    }
    if (cfg.web.bind.empty()) {
        errors.push_back("web.bind is empty; expected an IP address (e.g. 127.0.0.1)");
    }
    if (cfg.daemon.heartbeat_interval_ms <= 0) {
        errors.push_back("daemon.heartbeat_interval_ms must be > 0");
    }
    if (cfg.daemon.heartbeat_timeout_ms <= cfg.daemon.heartbeat_interval_ms) {
        errors.push_back("daemon.heartbeat_timeout_ms must be > daemon.heartbeat_interval_ms");
    }
    if (cfg.daemon.service_name.empty()) {
        errors.push_back("daemon.service_name is empty");
    }
    if (cfg.memory.max_index_bytes == 0) {
        errors.push_back("memory.max_index_bytes must be > 0");
    }
    if (cfg.project_instructions.max_depth < 1) {
        errors.push_back("project_instructions.max_depth must be >= 1");
    }
    if (cfg.project_instructions.max_bytes == 0) {
        errors.push_back("project_instructions.max_bytes must be > 0");
    }
    if (cfg.project_instructions.max_total_bytes < cfg.project_instructions.max_bytes) {
        errors.push_back("project_instructions.max_total_bytes must be >= max_bytes");
    }
    for (const auto& fn : cfg.project_instructions.filenames) {
        if (fn.empty()) {
            errors.push_back("project_instructions.filenames contains empty entry");
            break;
        }
        if (fn.find('/') != std::string::npos || fn.find('\\') != std::string::npos) {
            errors.push_back("project_instructions.filenames entry must not contain path separator: " + fn);
            break;
        }
    }
    return errors;
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
                if (oj.contains("models_dev_provider_id") &&
                    oj["models_dev_provider_id"].is_string()) {
                    cfg.openai.models_dev_provider_id =
                        oj["models_dev_provider_id"].get<std::string>();
                }
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
            if (j.contains("memory") && j["memory"].is_object()) {
                const auto& mj = j["memory"];
                if (mj.contains("enabled") && mj["enabled"].is_boolean())
                    cfg.memory.enabled = mj["enabled"].get<bool>();
                if (mj.contains("max_index_bytes") && mj["max_index_bytes"].is_number_integer()) {
                    long long v = mj["max_index_bytes"].get<long long>();
                    if (v > 0) cfg.memory.max_index_bytes = static_cast<std::size_t>(v);
                }
            }
            if (j.contains("project_instructions") && j["project_instructions"].is_object()) {
                const auto& pj = j["project_instructions"];
                if (pj.contains("enabled") && pj["enabled"].is_boolean())
                    cfg.project_instructions.enabled = pj["enabled"].get<bool>();
                if (pj.contains("max_depth") && pj["max_depth"].is_number_integer()) {
                    int v = pj["max_depth"].get<int>();
                    if (v > 0) cfg.project_instructions.max_depth = v;
                }
                if (pj.contains("max_bytes") && pj["max_bytes"].is_number_integer()) {
                    long long v = pj["max_bytes"].get<long long>();
                    if (v > 0) cfg.project_instructions.max_bytes = static_cast<std::size_t>(v);
                }
                if (pj.contains("max_total_bytes") && pj["max_total_bytes"].is_number_integer()) {
                    long long v = pj["max_total_bytes"].get<long long>();
                    if (v > 0) cfg.project_instructions.max_total_bytes = static_cast<std::size_t>(v);
                }
                if (pj.contains("filenames") && pj["filenames"].is_array()) {
                    std::vector<std::string> fns;
                    for (const auto& v : pj["filenames"]) {
                        if (v.is_string()) {
                            std::string s = v.get<std::string>();
                            if (!s.empty()) fns.push_back(std::move(s));
                        }
                    }
                    // Empty array -> keep the struct's default list so ACECODE.md
                    // / AGENT.md / CLAUDE.md still work out of the box.
                    if (!fns.empty()) cfg.project_instructions.filenames = std::move(fns);
                }
                if (pj.contains("read_agent_md") && pj["read_agent_md"].is_boolean())
                    cfg.project_instructions.read_agent_md = pj["read_agent_md"].get<bool>();
                if (pj.contains("read_claude_md") && pj["read_claude_md"].is_boolean())
                    cfg.project_instructions.read_claude_md = pj["read_claude_md"].get<bool>();
            }
            if (j.contains("daemon") && j["daemon"].is_object()) {
                const auto& dj = j["daemon"];
                if (dj.contains("auto_start_on_double_click") && dj["auto_start_on_double_click"].is_boolean())
                    cfg.daemon.auto_start_on_double_click = dj["auto_start_on_double_click"].get<bool>();
                if (dj.contains("service_name") && dj["service_name"].is_string())
                    cfg.daemon.service_name = dj["service_name"].get<std::string>();
                if (dj.contains("heartbeat_interval_ms") && dj["heartbeat_interval_ms"].is_number_integer())
                    cfg.daemon.heartbeat_interval_ms = dj["heartbeat_interval_ms"].get<int>();
                if (dj.contains("heartbeat_timeout_ms") && dj["heartbeat_timeout_ms"].is_number_integer())
                    cfg.daemon.heartbeat_timeout_ms = dj["heartbeat_timeout_ms"].get<int>();
            }
            if (j.contains("web") && j["web"].is_object()) {
                const auto& wj = j["web"];
                if (wj.contains("enabled") && wj["enabled"].is_boolean())
                    cfg.web.enabled = wj["enabled"].get<bool>();
                if (wj.contains("bind") && wj["bind"].is_string())
                    cfg.web.bind = wj["bind"].get<std::string>();
                if (wj.contains("port") && wj["port"].is_number_integer())
                    cfg.web.port = wj["port"].get<int>();
                // static_dir is intentionally optional. null/missing -> embedded assets;
                // string -> filesystem path. Empty string is treated the same as null.
                if (wj.contains("static_dir") && wj["static_dir"].is_string())
                    cfg.web.static_dir = wj["static_dir"].get<std::string>();
            }
            if (j.contains("models_dev") && j["models_dev"].is_object()) {
                const auto& mj = j["models_dev"];
                if (mj.contains("allow_network") && mj["allow_network"].is_boolean())
                    cfg.models_dev.allow_network = mj["allow_network"].get<bool>();
                if (mj.contains("refresh_on_command_only") && mj["refresh_on_command_only"].is_boolean())
                    cfg.models_dev.refresh_on_command_only = mj["refresh_on_command_only"].get<bool>();
                if (mj.contains("user_override_path") && mj["user_override_path"].is_string()) {
                    std::string p = mj["user_override_path"].get<std::string>();
                    if (!p.empty()) cfg.models_dev.user_override_path = p;
                }
            }
            if (j.contains("input_history") && j["input_history"].is_object()) {
                const auto& ihj = j["input_history"];
                if (ihj.contains("enabled") && ihj["enabled"].is_boolean())
                    cfg.input_history.enabled = ihj["enabled"].get<bool>();
                if (ihj.contains("max_entries") && ihj["max_entries"].is_number_integer()) {
                    int v = ihj["max_entries"].get<int>();
                    if (v > 0) cfg.input_history.max_entries = v;
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
    if (cfg.openai.models_dev_provider_id.has_value() &&
        !cfg.openai.models_dev_provider_id->empty()) {
        j["openai"]["models_dev_provider_id"] = *cfg.openai.models_dev_provider_id;
    }
    j["copilot"]["model"] = cfg.copilot.model;
    j["context_window"] = cfg.context_window;
    j["max_sessions"] = cfg.max_sessions;

    if (!cfg.skills.disabled.empty() || !cfg.skills.external_dirs.empty()) {
        nlohmann::json sj = nlohmann::json::object();
        if (!cfg.skills.disabled.empty()) sj["disabled"] = cfg.skills.disabled;
        if (!cfg.skills.external_dirs.empty()) sj["external_dirs"] = cfg.skills.external_dirs;
        j["skills"] = sj;
    }

    {
        // Persist daemon and web sections so users discover the available
        // tunables; emit only fields that diverge from defaults to keep
        // the file readable.
        DaemonConfig dd;
        nlohmann::json dj = nlohmann::json::object();
        if (cfg.daemon.auto_start_on_double_click != dd.auto_start_on_double_click)
            dj["auto_start_on_double_click"] = cfg.daemon.auto_start_on_double_click;
        if (cfg.daemon.service_name != dd.service_name)
            dj["service_name"] = cfg.daemon.service_name;
        if (cfg.daemon.heartbeat_interval_ms != dd.heartbeat_interval_ms)
            dj["heartbeat_interval_ms"] = cfg.daemon.heartbeat_interval_ms;
        if (cfg.daemon.heartbeat_timeout_ms != dd.heartbeat_timeout_ms)
            dj["heartbeat_timeout_ms"] = cfg.daemon.heartbeat_timeout_ms;
        if (!dj.empty()) j["daemon"] = dj;

        WebConfig wd;
        nlohmann::json wj = nlohmann::json::object();
        if (cfg.web.enabled != wd.enabled)
            wj["enabled"] = cfg.web.enabled;
        if (cfg.web.bind != wd.bind)
            wj["bind"] = cfg.web.bind;
        if (cfg.web.port != wd.port)
            wj["port"] = cfg.web.port;
        if (!cfg.web.static_dir.empty())
            wj["static_dir"] = cfg.web.static_dir;
        if (!wj.empty()) j["web"] = wj;

        MemoryConfig mem_d;
        nlohmann::json memj = nlohmann::json::object();
        if (cfg.memory.enabled != mem_d.enabled)
            memj["enabled"] = cfg.memory.enabled;
        if (cfg.memory.max_index_bytes != mem_d.max_index_bytes)
            memj["max_index_bytes"] = cfg.memory.max_index_bytes;
        if (!memj.empty()) j["memory"] = memj;

        ProjectInstructionsConfig pi_d;
        nlohmann::json pij = nlohmann::json::object();
        if (cfg.project_instructions.enabled != pi_d.enabled)
            pij["enabled"] = cfg.project_instructions.enabled;
        if (cfg.project_instructions.max_depth != pi_d.max_depth)
            pij["max_depth"] = cfg.project_instructions.max_depth;
        if (cfg.project_instructions.max_bytes != pi_d.max_bytes)
            pij["max_bytes"] = cfg.project_instructions.max_bytes;
        if (cfg.project_instructions.max_total_bytes != pi_d.max_total_bytes)
            pij["max_total_bytes"] = cfg.project_instructions.max_total_bytes;
        if (cfg.project_instructions.filenames != pi_d.filenames)
            pij["filenames"] = cfg.project_instructions.filenames;
        if (cfg.project_instructions.read_agent_md != pi_d.read_agent_md)
            pij["read_agent_md"] = cfg.project_instructions.read_agent_md;
        if (cfg.project_instructions.read_claude_md != pi_d.read_claude_md)
            pij["read_claude_md"] = cfg.project_instructions.read_claude_md;
        if (!pij.empty()) j["project_instructions"] = pij;

        ModelsDevConfig md;
        nlohmann::json mdj = nlohmann::json::object();
        if (cfg.models_dev.allow_network != md.allow_network)
            mdj["allow_network"] = cfg.models_dev.allow_network;
        if (cfg.models_dev.refresh_on_command_only != md.refresh_on_command_only)
            mdj["refresh_on_command_only"] = cfg.models_dev.refresh_on_command_only;
        if (cfg.models_dev.user_override_path.has_value() &&
            !cfg.models_dev.user_override_path->empty())
            mdj["user_override_path"] = *cfg.models_dev.user_override_path;
        if (!mdj.empty()) j["models_dev"] = mdj;

        InputHistoryConfig ih_d;
        nlohmann::json ihj = nlohmann::json::object();
        if (cfg.input_history.enabled != ih_d.enabled)
            ihj["enabled"] = cfg.input_history.enabled;
        if (cfg.input_history.max_entries != ih_d.max_entries)
            ihj["max_entries"] = cfg.input_history.max_entries;
        if (!ihj.empty()) j["input_history"] = ihj;
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
