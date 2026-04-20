#include "configure_catalog.hpp"

#include "../utils/terminal_input.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <iostream>
#include <sstream>

namespace acecode {

namespace {

std::string lower(std::string v) {
    std::transform(v.begin(), v.end(), v.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return v;
}

bool contains_ci(const std::string& haystack, const std::string& needle) {
    if (needle.empty()) return true;
    return lower(haystack).find(lower(needle)) != std::string::npos;
}

std::string env_get_ci(const std::string& name) {
    if (const char* v = std::getenv(name.c_str())) return v;
#ifdef _WIN32
    if (const char* v = std::getenv(lower(name).c_str())) return v;
    std::string upper = name;
    std::transform(upper.begin(), upper.end(), upper.begin(),
                   [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    if (const char* v = std::getenv(upper.c_str())) return v;
#endif
    return {};
}

constexpr size_t kPageSize = 30;

void print_provider_page(const std::vector<const ProviderEntry*>& providers,
                         size_t page) {
    size_t pages = (providers.size() + kPageSize - 1) / kPageSize;
    if (pages == 0) pages = 1;
    size_t begin = page * kPageSize;
    size_t end = std::min(providers.size(), begin + kPageSize);
    std::cout << "\nProviders (" << (begin + 1) << "-" << end << " of "
              << providers.size() << ", page " << (page + 1) << "/" << pages << "):\n";
    for (size_t i = begin; i < end; ++i) {
        std::cout << "  " << (i + 1) << ". " << format_provider_row(*providers[i]) << "\n";
    }
    std::cout << "Commands: <number> select, /<query> filter, n/p next/prev page, q quit\n";
}

void print_model_page(const std::vector<const ModelEntry*>& models, size_t page) {
    size_t pages = (models.size() + kPageSize - 1) / kPageSize;
    if (pages == 0) pages = 1;
    size_t begin = page * kPageSize;
    size_t end = std::min(models.size(), begin + kPageSize);
    std::cout << "\nModels (" << (begin + 1) << "-" << end << " of "
              << models.size() << ", page " << (page + 1) << "/" << pages << "):\n";
    std::cout << "  0. <Custom model id...>\n";
    for (size_t i = begin; i < end; ++i) {
        std::cout << "  " << (i + 1) << ". " << format_model_row(*models[i]) << "\n";
    }
    std::cout << "Commands: <number> select (0 = custom), /<query> filter, n/p next/prev page, q quit\n";
}

} // namespace

std::vector<const ProviderEntry*>
filter_providers(const std::vector<const ProviderEntry*>& src, const std::string& query) {
    if (query.empty()) return src;
    std::vector<const ProviderEntry*> out;
    out.reserve(src.size());
    for (auto* p : src) {
        if (contains_ci(p->id, query) || contains_ci(p->name, query)) out.push_back(p);
    }
    return out;
}

std::vector<const ModelEntry*>
filter_models(const std::vector<const ModelEntry*>& src, const std::string& query) {
    if (query.empty()) return src;
    std::vector<const ModelEntry*> out;
    out.reserve(src.size());
    for (auto* m : src) {
        if (contains_ci(m->id, query)) out.push_back(m);
    }
    return out;
}

std::string format_provider_row(const ProviderEntry& p) {
    std::ostringstream oss;
    oss << p.id;
    if (p.id != p.name) oss << "  (" << p.name << ")";
    oss << "  models=" << p.models.size();
    if (p.doc) oss << "  doc=" << *p.doc;
    return oss.str();
}

std::string format_model_row(const ModelEntry& m) {
    std::ostringstream oss;
    oss << m.id;
    auto ctx = format_context(m.context);
    if (!ctx.empty()) oss << "  ctx=" << ctx;
    auto cost = format_cost(m.cost_input, m.cost_output);
    if (!cost.empty()) oss << "  " << cost;
    auto caps = format_capabilities(m);
    if (!caps.empty()) oss << "  " << caps;
    return oss.str();
}

std::string format_model_summary(const ModelEntry& m) {
    std::ostringstream oss;
    oss << "Selected model: " << m.id;
    if (!m.name.empty() && m.name != m.id) oss << " (" << m.name << ")";
    auto ctx = format_context(m.context);
    if (!ctx.empty()) oss << "\n  context:       " << ctx << " tokens";
    if (m.max_output) oss << "\n  max output:    " << *m.max_output << " tokens";
    auto cost = format_cost(m.cost_input, m.cost_output);
    if (!cost.empty()) oss << "\n  cost:          " << cost << " per 1M tokens";
    auto caps = format_capabilities(m);
    if (!caps.empty()) oss << "\n  capabilities:  " << caps;
    if (!m.input_modalities.empty()) {
        oss << "\n  input:         ";
        for (size_t i = 0; i < m.input_modalities.size(); ++i) {
            if (i) oss << ", ";
            oss << m.input_modalities[i];
        }
    }
    if (m.knowledge_cutoff) oss << "\n  knowledge:     " << *m.knowledge_cutoff;
    return oss.str();
}

std::string format_source_line(const AppConfig& cfg) {
    if (cfg.provider == "copilot") return "copilot";
    if (cfg.openai.models_dev_provider_id.has_value() &&
        !cfg.openai.models_dev_provider_id->empty()) {
        return "openai (provider=" + *cfg.openai.models_dev_provider_id + " via models.dev)";
    }
    return "openai (custom)";
}

std::optional<EnvKeyHit> lookup_env_key(const ProviderEntry& provider) {
    for (const auto& env : provider.env) {
        std::string v = env_get_ci(env);
        if (!v.empty()) {
            return EnvKeyHit{env, v};
        }
    }
    return std::nullopt;
}

const ProviderEntry* run_provider_picker(
    const std::vector<const ProviderEntry*>& providers_in) {
    if (providers_in.empty()) {
        std::cout << "No openai-compatible providers available in catalog.\n";
        return nullptr;
    }
    std::vector<const ProviderEntry*> providers = providers_in;
    std::string query;
    size_t page = 0;

    while (true) {
        size_t pages = (providers.size() + kPageSize - 1) / kPageSize;
        if (pages == 0) pages = 1;
        if (page >= pages) page = pages - 1;
        print_provider_page(providers, page);
        std::cout << "> " << std::flush;

        std::string input;
        if (!std::getline(std::cin, input)) return nullptr;
        // Trim
        while (!input.empty() && std::isspace(static_cast<unsigned char>(input.back()))) input.pop_back();
        size_t lead = 0;
        while (lead < input.size() && std::isspace(static_cast<unsigned char>(input[lead]))) ++lead;
        input.erase(0, lead);

        if (input.empty()) continue;
        if (input == "q" || input == "Q") return nullptr;
        if (input == "n" || input == "N") {
            if (page + 1 < pages) ++page;
            continue;
        }
        if (input == "p" || input == "P") {
            if (page > 0) --page;
            continue;
        }
        if (input.front() == '/') {
            query = input.substr(1);
            providers = filter_providers(providers_in, query);
            page = 0;
            if (providers.empty()) {
                std::cout << "No matches for '" << query << "'. Type / to clear filter.\n";
            }
            continue;
        }
        try {
            int n = std::stoi(input);
            if (n >= 1 && n <= static_cast<int>(providers.size())) {
                return providers[n - 1];
            }
        } catch (...) {}
        std::cout << "Unrecognised input.\n";
    }
}

ModelPickerResult run_model_picker(const ProviderEntry& provider,
                                   const std::string& default_model_id) {
    ModelPickerResult result;

    std::vector<const ModelEntry*> all_models;
    all_models.reserve(provider.models.size());
    for (const auto& m : provider.models) all_models.push_back(&m);

    std::vector<const ModelEntry*> models = all_models;
    std::string query;
    size_t page = 0;

    if (all_models.empty()) {
        std::cout << "Provider '" << provider.id << "' has no catalog models. Falling back to custom input.\n";
        std::string id = read_line("Model id", default_model_id);
        result.custom = true;
        result.model_id = id;
        result.cancelled = id.empty();
        return result;
    }

    if (!default_model_id.empty()) {
        for (size_t i = 0; i < all_models.size(); ++i) {
            if (all_models[i]->id == default_model_id) {
                page = i / kPageSize;
                break;
            }
        }
    }

    while (true) {
        size_t pages = (models.size() + kPageSize - 1) / kPageSize;
        if (pages == 0) pages = 1;
        if (page >= pages) page = pages - 1;
        print_model_page(models, page);
        if (!default_model_id.empty()) {
            std::cout << "(current: " << default_model_id << ")\n";
        }
        std::cout << "> " << std::flush;

        std::string input;
        if (!std::getline(std::cin, input)) {
            result.cancelled = true;
            return result;
        }
        while (!input.empty() && std::isspace(static_cast<unsigned char>(input.back()))) input.pop_back();
        size_t lead = 0;
        while (lead < input.size() && std::isspace(static_cast<unsigned char>(input[lead]))) ++lead;
        input.erase(0, lead);

        if (input.empty()) continue;
        if (input == "q" || input == "Q") {
            result.cancelled = true;
            return result;
        }
        if (input == "n" || input == "N") {
            if (page + 1 < pages) ++page;
            continue;
        }
        if (input == "p" || input == "P") {
            if (page > 0) --page;
            continue;
        }
        if (input.front() == '/') {
            query = input.substr(1);
            models = filter_models(all_models, query);
            page = 0;
            if (models.empty()) std::cout << "No matches.\n";
            continue;
        }
        try {
            int n = std::stoi(input);
            if (n == 0) {
                std::string id = read_line("Custom model id", default_model_id);
                result.custom = true;
                result.model_id = id;
                result.cancelled = id.empty();
                return result;
            }
            if (n >= 1 && n <= static_cast<int>(models.size())) {
                result.selected = models[n - 1];
                result.model_id = models[n - 1]->id;
                return result;
            }
        } catch (...) {}
        std::cout << "Unrecognised input.\n";
    }
}

bool configure_openai_via_catalog(AppConfig& cfg) {
    auto providers = openai_compat_providers();
    if (providers.empty()) {
        std::cout << "models.dev catalog is unavailable; cannot browse.\n";
        return false;
    }

    std::cout << "\n--- Browse models.dev catalog ---\n";
    const ProviderEntry* provider = run_provider_picker(providers);
    if (!provider) {
        std::cout << "Cancelled.\n";
        return false;
    }

    std::cout << "\nSelected provider: " << provider->id;
    if (provider->id != provider->name) std::cout << " (" << provider->name << ")";
    std::cout << std::endl;

    std::string base_default = provider->base_url.value_or(cfg.openai.base_url);
    cfg.openai.base_url = read_line("Base URL", base_default);

    auto env_hit = lookup_env_key(*provider);
    std::string current_key = cfg.openai.api_key;
    if (env_hit) {
        std::cout << "API Key (env: " << env_hit->env_name
                  << " found, leave blank to keep using the env value): ";
        std::string typed;
        std::getline(std::cin, typed);
        if (typed.empty()) {
            cfg.openai.api_key.clear();
        } else {
            cfg.openai.api_key = typed;
        }
    } else {
        std::ostringstream env_names;
        for (size_t i = 0; i < provider->env.size(); ++i) {
            if (i) env_names << " / ";
            env_names << provider->env[i];
        }
        if (provider->env.empty()) env_names << "(none documented)";
        std::cout << "No env var (" << env_names.str() << ") found.\n";
        cfg.openai.api_key = read_password("API Key", current_key);
    }

    cfg.provider = "openai";
    cfg.openai.models_dev_provider_id = provider->id;

    auto picked = run_model_picker(*provider, cfg.openai.model);
    if (picked.cancelled) {
        std::cout << "Model selection cancelled — keeping previous model: "
                  << cfg.openai.model << "\n";
    } else {
        cfg.openai.model = picked.model_id;
        if (picked.selected) {
            std::cout << "\n" << format_model_summary(*picked.selected) << "\n";
        } else {
            std::cout << "\nUsing custom model id: " << picked.model_id << "\n";
        }
    }
    return true;
}

} // namespace acecode
