#include "models_dev_catalog.hpp"

#include "../provider/models_dev_registry.hpp"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <mutex>
#include <sstream>

namespace acecode {

namespace {

std::optional<int> as_int(const nlohmann::json& v) {
    if (v.is_number_integer()) return v.get<int>();
    if (v.is_number_unsigned()) return static_cast<int>(v.get<unsigned int>());
    if (v.is_number_float()) return static_cast<int>(v.get<double>());
    return std::nullopt;
}

std::optional<double> as_double(const nlohmann::json& v) {
    if (v.is_number()) return v.get<double>();
    return std::nullopt;
}

std::vector<std::string> as_string_array(const nlohmann::json& v) {
    std::vector<std::string> out;
    if (!v.is_array()) return out;
    out.reserve(v.size());
    for (const auto& item : v) {
        if (item.is_string()) out.push_back(item.get<std::string>());
    }
    return out;
}

std::optional<std::string> first_string(const nlohmann::json& obj,
                                        std::initializer_list<const char*> keys) {
    if (!obj.is_object()) return std::nullopt;
    for (const char* k : keys) {
        auto it = obj.find(k);
        if (it != obj.end() && it->is_string()) {
            std::string v = it->get<std::string>();
            if (!v.empty()) return v;
        }
    }
    return std::nullopt;
}

bool first_bool(const nlohmann::json& obj, const char* key, bool fallback) {
    auto it = obj.find(key);
    if (it != obj.end() && it->is_boolean()) return it->get<bool>();
    return fallback;
}

ModelEntry parse_model(const std::string& id, const nlohmann::json& mj) {
    ModelEntry m;
    m.id = id;
    if (auto name = first_string(mj, {"name"})) m.name = *name;
    if (m.name.empty()) m.name = id;

    auto limit_it = mj.find("limit");
    if (limit_it != mj.end() && limit_it->is_object()) {
        if (auto ctx = limit_it->find("context"); ctx != limit_it->end())
            m.context = as_int(*ctx);
        if (auto out = limit_it->find("output"); out != limit_it->end())
            m.max_output = as_int(*out);
    }

    auto cost_it = mj.find("cost");
    if (cost_it != mj.end() && cost_it->is_object()) {
        if (auto ci = cost_it->find("input"); ci != cost_it->end())
            m.cost_input = as_double(*ci);
        if (auto co = cost_it->find("output"); co != cost_it->end())
            m.cost_output = as_double(*co);
    }

    m.reasoning = first_bool(mj, "reasoning", false);
    m.tool_call = first_bool(mj, "tool_call", false);
    m.attachment = first_bool(mj, "attachment", false);
    m.deprecated = first_bool(mj, "deprecated", false);

    auto mod_it = mj.find("modalities");
    if (mod_it != mj.end() && mod_it->is_object()) {
        if (auto ii = mod_it->find("input"); ii != mod_it->end())
            m.input_modalities = as_string_array(*ii);
        if (auto oi = mod_it->find("output"); oi != mod_it->end())
            m.output_modalities = as_string_array(*oi);
    }

    if (auto k = first_string(mj, {"knowledge"})) m.knowledge_cutoff = *k;

    return m;
}

void parse_models_field(const nlohmann::json& models, std::vector<ModelEntry>& out) {
    if (models.is_object()) {
        for (auto it = models.begin(); it != models.end(); ++it) {
            if (!it->is_object()) continue;
            std::string id = it.key();
            if (auto idj = it->find("id"); idj != it->end() && idj->is_string()) {
                id = idj->get<std::string>();
            }
            out.push_back(parse_model(id, *it));
        }
    } else if (models.is_array()) {
        for (const auto& item : models) {
            if (!item.is_object()) continue;
            std::string id;
            if (auto idj = item.find("id"); idj != item.end() && idj->is_string()) {
                id = idj->get<std::string>();
            }
            if (id.empty()) continue;
            out.push_back(parse_model(id, item));
        }
    }
}

ProviderEntry parse_provider(const std::string& key, const nlohmann::json& pj) {
    ProviderEntry p;
    p.id = key;
    if (auto idj = pj.find("id"); idj != pj.end() && idj->is_string()) {
        p.id = idj->get<std::string>();
    }
    if (auto name = first_string(pj, {"name"})) p.name = *name;
    if (p.name.empty()) p.name = p.id;

    if (auto env_it = pj.find("env"); env_it != pj.end()) {
        if (env_it->is_array()) {
            p.env = as_string_array(*env_it);
        } else if (env_it->is_string()) {
            p.env.push_back(env_it->get<std::string>());
        }
    }

    if (auto doc = first_string(pj, {"doc"})) p.doc = *doc;

    // models.dev's `api` field is a plain string (the OpenAI-compatible base URL).
    // Older / hand-curated payloads sometimes used a nested form
    // (`api.openai.base` or `api.base`); accept either shape so a future schema
    // tweak doesn't break the catalog.
    auto api_it = pj.find("api");
    if (api_it != pj.end()) {
        if (api_it->is_string()) {
            std::string s = api_it->get<std::string>();
            if (!s.empty()) p.base_url = s;
        } else if (api_it->is_object()) {
            auto openai_it = api_it->find("openai");
            if (openai_it != api_it->end() && openai_it->is_object()) {
                if (auto base = first_string(*openai_it, {"base", "base_url"})) {
                    p.base_url = *base;
                }
            }
            if (!p.base_url.has_value()) {
                if (auto base = first_string(*api_it, {"base", "base_url"})) {
                    p.base_url = *base;
                }
            }
        }
    }
    if (!p.base_url.has_value()) {
        if (auto base = first_string(pj, {"base_url"})) p.base_url = *base;
    }
    p.openai_compatible = p.base_url.has_value();

    if (auto models = pj.find("models"); models != pj.end()) {
        parse_models_field(*models, p.models);
        std::sort(p.models.begin(), p.models.end(),
                  [](const ModelEntry& a, const ModelEntry& b) { return a.id < b.id; });
    }

    return p;
}

struct CatalogCache {
    std::shared_ptr<const nlohmann::json> source;
    std::vector<ProviderEntry> providers;
    unsigned long long version = 0;
};

std::mutex& cache_mutex() {
    static std::mutex m;
    return m;
}

CatalogCache& cache_storage() {
    static CatalogCache c;
    return c;
}

const std::vector<ProviderEntry>& refresh_cache_if_stale() {
    std::lock_guard<std::mutex> lk(cache_mutex());
    auto current = current_registry();
    auto& cache = cache_storage();
    if (cache.source.get() == current.get() && cache.source) {
        return cache.providers;
    }
    cache.source = current;
    cache.providers = current ? build_catalog(*current) : std::vector<ProviderEntry>{};
    std::sort(cache.providers.begin(), cache.providers.end(),
              [](const ProviderEntry& a, const ProviderEntry& b) {
                  return a.name < b.name;
              });
    ++cache.version;
    return cache.providers;
}

} // namespace

std::vector<ProviderEntry> build_catalog(const nlohmann::json& registry) {
    std::vector<ProviderEntry> out;
    if (!registry.is_object()) return out;
    out.reserve(registry.size());
    for (auto it = registry.begin(); it != registry.end(); ++it) {
        if (!it->is_object()) continue;
        out.push_back(parse_provider(it.key(), *it));
    }
    return out;
}

const std::vector<ProviderEntry>& all_providers() {
    return refresh_cache_if_stale();
}

std::vector<const ProviderEntry*> openai_compat_providers() {
    const auto& providers = all_providers();
    std::vector<const ProviderEntry*> out;
    out.reserve(providers.size());
    for (const auto& p : providers) {
        if (p.openai_compatible) out.push_back(&p);
    }
    return out;
}

const ProviderEntry* find_provider(const std::string& id) {
    for (const auto& p : all_providers()) {
        if (p.id == id) return &p;
    }
    return nullptr;
}

const ModelEntry* find_model(const ProviderEntry& provider, const std::string& model_id) {
    for (const auto& m : provider.models) {
        if (m.id == model_id) return &m;
    }
    return nullptr;
}

std::string format_context(const std::optional<int>& tokens) {
    if (!tokens.has_value() || *tokens <= 0) return "";
    int n = *tokens;
    std::ostringstream oss;
    if (n >= 1000000 && n % 1000000 == 0) {
        oss << (n / 1000000) << "M";
    } else if (n >= 1000000) {
        oss << std::fixed << std::setprecision(1) << (n / 1000000.0) << "M";
    } else if (n >= 1000) {
        if (n % 1000 == 0) oss << (n / 1000) << "k";
        else oss << std::fixed << std::setprecision(1) << (n / 1000.0) << "k";
    } else {
        oss << n;
    }
    return oss.str();
}

namespace {
std::string trim_zero(std::string s) {
    if (s.find('.') == std::string::npos) return s;
    while (!s.empty() && s.back() == '0') s.pop_back();
    if (!s.empty() && s.back() == '.') s.pop_back();
    return s;
}
}

std::string format_cost(const std::optional<double>& input,
                        const std::optional<double>& output) {
    if (!input.has_value() && !output.has_value()) return "";
    auto fmt = [](double v) {
        std::ostringstream oss;
        oss << std::fixed << std::setprecision(2) << v;
        return trim_zero(oss.str());
    };
    std::ostringstream oss;
    oss << "in=$" << (input ? fmt(*input) : std::string("?"))
        << "/out=$" << (output ? fmt(*output) : std::string("?"));
    return oss.str();
}

std::string format_capabilities(const ModelEntry& model) {
    std::vector<std::string> tags;
    if (model.tool_call) tags.push_back("tools");
    if (model.attachment) tags.push_back("vision");
    if (model.reasoning) tags.push_back("reasoning");
    if (model.deprecated) tags.push_back("deprecated");
    if (tags.empty()) return "";
    std::ostringstream oss;
    oss << "[";
    for (size_t i = 0; i < tags.size(); ++i) {
        if (i) oss << ", ";
        oss << tags[i];
    }
    oss << "]";
    return oss.str();
}

unsigned long long catalog_version() {
    refresh_cache_if_stale();
    std::lock_guard<std::mutex> lk(cache_mutex());
    return cache_storage().version;
}

} // namespace acecode
