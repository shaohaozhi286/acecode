#include "configure.hpp"
#include "configure_catalog.hpp"
#include "configure_picker.hpp"
#include "codex/codex_app_server_client.hpp"
#include "codex/codex_model_catalog.hpp"
#include "config/config.hpp"
#include "auth/github_auth.hpp"
#include "network/proxy_resolver.hpp"
#include "utils/logger.hpp"
#include "utils/models_dev_catalog.hpp"
#include "utils/terminal_input.hpp"

#include <iostream>
#include <string>
#include <sstream>
#include <vector>
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <cpr/cpr.h>
#include <cpr/ssl_options.h>
#include <nlohmann/json.hpp>

namespace acecode {

// Mask an API key for display: "****" + last 4 chars
static std::string mask_key(const std::string& key) {
    if (key.empty()) return "(not set)";
    if (key.size() > 4) {
        return "****" + key.substr(key.size() - 4);
    }
    return "****";
}

static std::string configured_saved_model_name(const AppConfig& cfg) {
    if (cfg.provider == "codex") return "codex";
    if (cfg.provider == "copilot") return "copilot";
    if (cfg.openai.models_dev_provider_id.has_value() &&
        !cfg.openai.models_dev_provider_id->empty()) {
        return *cfg.openai.models_dev_provider_id;
    }
    return "openai";
}

static ModelProfile configured_profile_from_current_fields(const AppConfig& cfg) {
    ModelProfile profile;
    profile.name = configured_saved_model_name(cfg);
    if (cfg.provider == "openai") {
        profile.provider = "openai";
        profile.base_url = cfg.openai.base_url;
        profile.api_key = cfg.openai.api_key;
        profile.model = cfg.openai.model;
        profile.models_dev_provider_id = cfg.openai.models_dev_provider_id;
    } else if (cfg.provider == "codex") {
        profile.provider = "codex";
        profile.model = cfg.codex.model;
    } else {
        profile.provider = "copilot";
        profile.model = cfg.copilot.model;
    }
    return profile;
}

static void upsert_configured_saved_model(AppConfig& cfg) {
    ModelProfile profile = configured_profile_from_current_fields(cfg);
    for (auto& existing : cfg.saved_models) {
        if (existing.name == profile.name) {
            existing = profile;
            cfg.default_model_name = profile.name;
            return;
        }
    }
    cfg.saved_models.push_back(profile);
    cfg.default_model_name = profile.name;
}

static void configure_copilot(AppConfig& cfg) {
    std::cout << "\n--- Copilot Configuration ---\n" << std::endl;

    // Authentication first (needed to fetch available models)
    std::string github_token = load_github_token();
    if (github_token.empty()) {
        std::cout << "GitHub authentication required." << std::endl;
    } else {
        std::cout << "GitHub authentication: Already authenticated." << std::endl;
        if (read_confirm("Re-authenticate?", false)) {
            github_token.clear(); // force re-auth below
        }
    }

    if (github_token.empty()) {
        std::cout << "\nStarting GitHub authentication..." << std::endl;
        DeviceCodeResponse dc = request_device_code();
        if (dc.device_code.empty()) {
            std::cerr << "Error: Failed to request device code from GitHub." << std::endl;
            return;
        }

        std::cout << "\n  Please open: " << dc.verification_uri << std::endl;
        std::cout << "  Enter code:  " << dc.user_code << std::endl;
        std::cout << std::endl;

        github_token = poll_for_access_token(
            dc.device_code, dc.interval, dc.expires_in,
            [](const std::string& status) {
                std::cout << "  " << status << std::endl;
            }
        );

        if (github_token.empty()) {
            std::cerr << "Authentication failed or timed out." << std::endl;
            return;
        }

        save_github_token(github_token);
        std::cout << "Authentication successful!" << std::endl;
    }

    // Fetch available models from Copilot API
    std::cout << "\nFetching available models..." << std::endl;
    CopilotToken ct = exchange_copilot_token(github_token);

    std::vector<std::string> model_ids;
    if (!ct.token.empty()) {
        static const std::string kCopilotModelsUrl =
            "https://api.githubcopilot.com/models";
        auto proxy_opts = network::proxy_options_for(kCopilotModelsUrl);
        cpr::Response r = cpr::Get(
            cpr::Url{kCopilotModelsUrl},
            cpr::Header{
                {"Authorization", "Bearer " + ct.token},
                {"Editor-Version", "acecode/0.1.0"},
                {"Editor-Plugin-Version", "acecode/0.1.0"},
                {"Copilot-Integration-Id", "vscode-chat"},
                {"Openai-Intent", "conversation-panel"}
            },
            network::build_ssl_options(proxy_opts),
            proxy_opts.proxies,
            proxy_opts.auth,
            cpr::Timeout{10000}
        );

        if (r.status_code == 200) {
            try {
                auto j = nlohmann::json::parse(r.text);
                if (j.contains("data") && j["data"].is_array()) {
                    for (const auto& m : j["data"]) {
                        if (!m.contains("id") || !m["id"].is_string()) continue;
                        // Filter to chat-capable models only
                        if (m.contains("capabilities")) {
                            const auto& caps = m["capabilities"];
                            if (caps.contains("type") && caps["type"].is_string()
                                && caps["type"].get<std::string>() != "chat") {
                                continue;
                            }
                        }
                        model_ids.push_back(m["id"].get<std::string>());
                    }
                }
            } catch (...) {}
        }
    }

    // Catalog augmentation: enrich each id with metadata from models.dev
    // (`github-copilot` provider) when available, and fall back to the catalog
    // list entirely when GitHub `/models` returned nothing.
    const ProviderEntry* copilot_catalog = find_provider("github-copilot");

    auto label_for = [&](const std::string& id) -> std::string {
        if (!copilot_catalog) return id;
        const ModelEntry* m = find_model(*copilot_catalog, id);
        if (!m) return id + "  (no metadata)";
        std::ostringstream oss;
        oss << id;
        auto ctx = format_context(m->context);
        if (!ctx.empty()) oss << "  ctx=" << ctx;
        auto caps = format_capabilities(*m);
        if (!caps.empty()) oss << "  " << caps;
        return oss.str();
    };

    std::vector<std::string> labels;
    std::vector<std::string> choice_ids;
    if (!model_ids.empty()) {
        for (const auto& id : model_ids) {
            labels.push_back(label_for(id));
            choice_ids.push_back(id);
        }
    } else if (copilot_catalog && !copilot_catalog->models.empty()) {
        std::cerr << "Warning: GitHub /models unavailable, falling back to catalog list." << std::endl;
        for (const auto& m : copilot_catalog->models) {
            labels.push_back(label_for(m.id));
            choice_ids.push_back(m.id);
        }
    }

    if (!choice_ids.empty()) {
        int default_idx = 0;
        for (size_t i = 0; i < choice_ids.size(); ++i) {
            if (choice_ids[i] == cfg.copilot.model) {
                default_idx = static_cast<int>(i);
                break;
            }
        }

        std::vector<PickerItem> items;
        items.reserve(choice_ids.size());
        for (size_t i = 0; i < choice_ids.size(); ++i) {
            PickerItem it;
            it.label = choice_ids[i];
            // labels[i] already starts with the id; strip it so the helper
            // renders id as the bold label and only the metadata as secondary.
            if (labels[i].rfind(choice_ids[i], 0) == 0) {
                std::string rest = labels[i].substr(choice_ids[i].size());
                while (!rest.empty() && rest.front() == ' ') rest.erase(rest.begin());
                it.secondary = rest;
            } else {
                it.secondary = labels[i];
            }
            items.push_back(std::move(it));
        }

        PickerOptions opts;
        opts.title = "Select a Copilot model";
        opts.page_size = 30;
        opts.allow_custom = true;
        opts.default_index = static_cast<size_t>(default_idx);

        PickerResult r = run_ftxui_picker(items, opts);
        if (r.cancelled) {
            std::cout << "Model selection cancelled — keeping: "
                      << cfg.copilot.model << "\n";
        } else if (r.custom) {
            cfg.copilot.model = read_line("Custom model id", cfg.copilot.model);
        } else {
            cfg.copilot.model = choice_ids[r.index];
        }
    } else {
        std::cerr << "Warning: Could not fetch model list from Copilot API and no catalog entry available." << std::endl;
        cfg.copilot.model = read_line("Model", cfg.copilot.model);
    }
}

static std::string codex_account_label(const codex::AccountInfo& account) {
    if (!account.present) return "(not logged in)";
    std::ostringstream oss;
    oss << account.type;
    if (!account.email.empty()) oss << " " << account.email;
    if (!account.plan_type.empty()) oss << " (" << account.plan_type << ")";
    return oss.str();
}

static bool wait_for_codex_login(codex::AppServerClient& client,
                                 const std::string& login_id) {
    std::mutex mu;
    std::condition_variable cv;
    bool done = false;

    client.set_notification_handler(
        [&](const std::string& method, const nlohmann::json& params) {
            if (method != "account/login/completed") return;
            if (!login_id.empty() && params.contains("loginId") &&
                params["loginId"].is_string() &&
                params["loginId"].get<std::string>() != login_id) {
                return;
            }
            {
                std::lock_guard<std::mutex> lk(mu);
                done = true;
            }
            cv.notify_all();
        });

    std::unique_lock<std::mutex> lk(mu);
    for (int elapsed = 0; elapsed < 600; ++elapsed) {
        if (cv.wait_for(lk, std::chrono::seconds(1), [&] { return done; })) {
            return true;
        }
        if ((elapsed + 1) % 10 == 0) {
            std::cout << "  Waiting for Codex login..." << std::endl;
        }
    }
    return false;
}

static void configure_codex(AppConfig& cfg) {
    std::cout << "\n--- Codex Configuration ---\n" << std::endl;

    codex::AppServerClient client;
    std::string error;
    if (!client.start(&error) || !client.initialize(&error)) {
        std::cerr << "Error: Failed to start `codex app-server`: "
                  << error << std::endl;
        std::cerr << "Install or update the official Codex CLI, then retry."
                  << std::endl;
        return;
    }

    auto account = client.read_account(false, &error);
    if (!account.has_value()) {
        std::cerr << "Warning: Codex account check failed: " << error << std::endl;
    } else if (account->present) {
        std::cout << "Codex authentication: "
                  << codex_account_label(*account) << std::endl;
        if (read_confirm("Re-authenticate?", false)) {
            account->present = false;
        }
    } else {
        std::cout << "Codex authentication required." << std::endl;
    }

    if (!account.has_value() || !account->present) {
        std::cout << "\nStarting Codex ChatGPT device login..." << std::endl;
        auto login = client.start_device_login(&error);
        if (!login.has_value()) {
            std::cerr << "Error: Failed to start Codex login: "
                      << error << std::endl;
            return;
        }

        std::string url = !login->verification_url.empty()
            ? login->verification_url
            : login->auth_url;
        std::cout << "\n  Please open: " << url << std::endl;
        if (!login->user_code.empty()) {
            std::cout << "  Enter code:  " << login->user_code << std::endl;
        }
        std::cout << std::endl;

        if (!wait_for_codex_login(client, login->login_id)) {
            std::cerr << "Authentication timed out." << std::endl;
            return;
        }

        account = client.read_account(true, &error);
        if (!account.has_value() || !account->present) {
            std::cerr << "Authentication did not produce a Codex account.";
            if (!error.empty()) std::cerr << " " << error;
            std::cerr << std::endl;
            return;
        }
        std::cout << "Authentication successful: "
                  << codex_account_label(*account) << std::endl;
    }

    std::cout << "\nFetching available Codex models..." << std::endl;
    auto models = client.list_models(false, &error);
    if (!models.empty()) {
        std::vector<PickerItem> items;
        std::vector<std::string> ids;
        items.reserve(models.size());
        ids.reserve(models.size());
        std::size_t default_index = 0;
        for (std::size_t i = 0; i < models.size(); ++i) {
            const auto& m = models[i];
            PickerItem item;
            item.label = m.id;
            std::ostringstream secondary;
            bool has_secondary = false;
            if (!m.display_name.empty() && m.display_name != m.id) {
                secondary << m.display_name;
                has_secondary = true;
            }
            if (!m.description.empty()) {
                if (has_secondary) secondary << "  ";
                secondary << m.description;
                has_secondary = true;
            }
            int context = codex::context_window_for_model(m.id);
            if (context > 0) {
                if (has_secondary) secondary << "  ";
                secondary << "ctx=" << format_context(context);
                has_secondary = true;
            }
            if (m.is_default) {
                if (has_secondary) secondary << "  ";
                secondary << "default";
            }
            item.secondary = secondary.str();
            items.push_back(std::move(item));
            ids.push_back(m.id);
            if (m.id == cfg.codex.model || (cfg.codex.model.empty() && m.is_default)) {
                default_index = i;
            }
        }

        PickerOptions opts;
        opts.title = "Select a Codex model";
        opts.page_size = 30;
        opts.allow_custom = true;
        opts.default_index = default_index;

        PickerResult r = run_ftxui_picker(items, opts);
        if (r.cancelled) {
            std::cout << "Model selection cancelled - keeping: "
                      << cfg.codex.model << "\n";
        } else if (r.custom) {
            cfg.codex.model = read_line("Custom model id", cfg.codex.model);
        } else {
            cfg.codex.model = ids[r.index];
        }
    } else {
        std::cerr << "Warning: Could not fetch Codex model list";
        if (!error.empty()) std::cerr << ": " << error;
        std::cerr << std::endl;
        cfg.codex.model = read_line("Model", cfg.codex.model);
    }

    cfg.provider = "codex";
    cfg.openai.models_dev_provider_id.reset();
}

static void configure_openai(AppConfig& cfg) {
    std::cout << "\n--- OpenAI Compatible Configuration ---\n" << std::endl;

    // Base URL
    cfg.openai.base_url = read_line("Base URL", cfg.openai.base_url);

    // API Key (masked)
    cfg.openai.api_key = read_password("API Key", cfg.openai.api_key);

    // Model
    cfg.openai.model = read_line("Model", cfg.openai.model);

    // Optional connection test
    if (read_confirm("\nTest connection?", true)) {
        std::cout << "Testing connection to " << cfg.openai.base_url << "/models ..." << std::endl;

        cpr::Header headers;
        if (!cfg.openai.api_key.empty()) {
            headers["Authorization"] = "Bearer " + cfg.openai.api_key;
        }

        const std::string models_url = cfg.openai.base_url + "/models";
        auto proxy_opts = network::proxy_options_for(models_url);
        auto r = cpr::Get(
            cpr::Url{models_url},
            headers,
            network::build_ssl_options(proxy_opts),
            proxy_opts.proxies,
            proxy_opts.auth,
            cpr::Timeout{10000}
        );

        if (r.status_code == 200) {
            std::cout << "Connection successful!" << std::endl;
        } else if (r.status_code == 0) {
            std::cerr << "Connection failed: " << r.error.message << std::endl;
            if (!read_confirm("Save configuration anyway?", true)) {
                return;
            }
        } else {
            std::cerr << "Connection failed: HTTP " << r.status_code << std::endl;
            if (!read_confirm("Save configuration anyway?", true)) {
                return;
            }
        }
    }
}

static void configure_upgrade_service(AppConfig& cfg) {
    std::cout << "\n--- Upgrade Service Configuration ---\n" << std::endl;

    const std::string current = normalize_upgrade_base_url(cfg.upgrade.base_url);
    while (true) {
        const std::string raw = read_line("Upgrade service URL", current);
        const std::string normalized = normalize_upgrade_base_url(raw);
        if (is_valid_upgrade_base_url(normalized)) {
            cfg.upgrade.base_url = normalized;
            return;
        }

        std::cerr << "Invalid upgrade service URL: use a non-empty http or https URL."
                  << std::endl;
        if (!read_confirm("Try again?", true)) {
            cfg.upgrade.base_url = current;
            std::cout << "Keeping upgrade service URL: " << current << std::endl;
            return;
        }
    }
}

int run_configure(const AppConfig& current_config) {
    AppConfig cfg = current_config;

    std::cout << "\n=== acecode Configuration Wizard ===\n" << std::endl;

    auto compat_providers = openai_compat_providers();
    bool catalog_ready = !compat_providers.empty();

    int default_provider = 0;
    if (cfg.provider == "openai") {
        default_provider = (cfg.openai.models_dev_provider_id.has_value() && catalog_ready) ? 1 : 2;
    }

    while (true) {
        std::vector<std::string> options = {
            "Copilot (GitHub)",
            catalog_ready ? ("Browse models.dev catalog (" +
                             std::to_string(compat_providers.size()) + " providers)")
                          : "Browse models.dev catalog (unavailable: no bundled snapshot found)",
            "Custom OpenAI compatible"
        };
        int choice = read_choice("Select provider:", options, default_provider);

        if (choice == 0) {
            cfg.provider = "copilot";
            // Switching to Copilot wipes the openai-side provider hint to avoid
            // stale source labels in future configure runs.
            cfg.openai.models_dev_provider_id.reset();
            configure_copilot(cfg);
            break;
        }
        if (choice == 1) {
            if (!catalog_ready) {
                std::cout << "Catalog unavailable; please pick another option.\n";
                continue;
            }
            if (configure_openai_via_catalog(cfg)) break;
            // user backed out — re-show menu
            continue;
        }
        cfg.provider = "openai";
        cfg.openai.models_dev_provider_id.reset();
        configure_openai(cfg);
        break;
    }

    upsert_configured_saved_model(cfg);
    configure_upgrade_service(cfg);

    // Configuration summary
    std::cout << "\n--- Configuration Summary ---" << std::endl;
    std::cout << "  Source:   " << format_source_line(cfg) << std::endl;
    std::cout << "  Provider: " << cfg.provider << std::endl;
    if (cfg.provider == "copilot") {
        std::cout << "  Model:    " << cfg.copilot.model << std::endl;
    } else {
        std::cout << "  Base URL: " << cfg.openai.base_url << std::endl;
        std::cout << "  API Key:  " << mask_key(cfg.openai.api_key) << std::endl;
        std::cout << "  Model:    " << cfg.openai.model << std::endl;
        if (cfg.openai.models_dev_provider_id.has_value()) {
            std::cout << "  Provider id (models.dev): "
                      << *cfg.openai.models_dev_provider_id << std::endl;
        }
    }
    std::cout << "  Saved model: " << cfg.default_model_name << std::endl;
    std::cout << "  Upgrade service URL: " << cfg.upgrade.base_url << std::endl;
    std::cout << std::endl;

    if (read_confirm("Save configuration?", true)) {
        save_config(cfg);
        LOG_INFO(std::string("configure: saved (") + format_source_line(cfg) +
                 ", model=" + (cfg.provider == "copilot"
                     ? cfg.copilot.model
                     : cfg.openai.model) + ")");
        std::cout << "Configuration saved!" << std::endl;
    } else {
        std::cout << "Configuration cancelled." << std::endl;
    }

    return 0;
}

} // namespace acecode
