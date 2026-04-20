#pragma once

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace acecode {

struct ModelEntry {
    std::string id;
    std::string name;                              // human label (falls back to id)
    std::optional<int> context;
    std::optional<int> max_output;
    std::optional<double> cost_input;              // USD / million tokens
    std::optional<double> cost_output;
    bool reasoning = false;
    bool tool_call = false;
    bool attachment = false;                       // vision / pdf
    bool deprecated = false;
    std::vector<std::string> input_modalities;
    std::vector<std::string> output_modalities;
    std::optional<std::string> knowledge_cutoff;
};

struct ProviderEntry {
    std::string id;                                // top-level key from api.json
    std::string name;
    std::vector<std::string> env;                  // suggested env var names
    std::optional<std::string> base_url;           // openai-compatible endpoint, when present
    std::optional<std::string> doc;
    bool openai_compatible = false;                // true iff base_url present
    std::vector<ModelEntry> models;
};

// Build a fresh catalog from the supplied registry JSON. Pure function — exposed
// for unit testing and for callers that want to render a catalog from a JSON
// blob other than the global registry.
std::vector<ProviderEntry> build_catalog(const nlohmann::json& registry);

// Cached view over the global registry (current_registry()). The cache is keyed
// off the shared_ptr identity, so calling this after refresh_registry_*() picks
// up the new data automatically.
const std::vector<ProviderEntry>& all_providers();

// Same as all_providers(), filtered to providers whose base_url is set.
std::vector<const ProviderEntry*> openai_compat_providers();

const ProviderEntry* find_provider(const std::string& id);
const ModelEntry* find_model(const ProviderEntry& provider, const std::string& model_id);

// Display helpers (safe to call with empty entries).
std::string format_context(const std::optional<int>& tokens);
std::string format_cost(const std::optional<double>& input,
                        const std::optional<double>& output);
std::string format_capabilities(const ModelEntry& model);

// Version of the catalog cache. Increments whenever the cache is rebuilt — used
// by tests and by long-lived UI components that want to invalidate state.
unsigned long long catalog_version();

} // namespace acecode
