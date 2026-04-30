#include "configure.hpp"
#include "configure_catalog.hpp"
#include "configure_picker.hpp"
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
    std::cout << std::endl;

    if (read_confirm("Save configuration?", true)) {
        save_config(cfg);
        LOG_INFO(std::string("configure: saved (") + format_source_line(cfg) +
                 ", model=" + (cfg.provider == "copilot" ? cfg.copilot.model : cfg.openai.model) + ")");
        std::cout << "Configuration saved!" << std::endl;
    } else {
        std::cout << "Configuration cancelled." << std::endl;
    }

    return 0;
}

} // namespace acecode
