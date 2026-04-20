#pragma once

#include "../config/config.hpp"
#include "../utils/models_dev_catalog.hpp"

#include <optional>
#include <string>
#include <vector>

namespace acecode {

// Apply substring filtering to a provider list. Case-insensitive match against
// id and name. Empty query returns the full input slice.
std::vector<const ProviderEntry*>
filter_providers(const std::vector<const ProviderEntry*>& src, const std::string& query);

// Apply substring filtering to a model list (matches against id only).
std::vector<const ModelEntry*>
filter_models(const std::vector<const ModelEntry*>& src, const std::string& query);

// Render a single provider list row in the wizard's standard format.
std::string format_provider_row(const ProviderEntry& p);

// Render a single model list row.
std::string format_model_row(const ModelEntry& m);

// Render the post-selection model summary block (multi-line). Used by configure
// and reused as a building block for /models lookup output formatting.
std::string format_model_summary(const ModelEntry& m);

// Source label written into the configuration summary line.
//   `cfg.openai.models_dev_provider_id` set     → "openai (provider=<id> via models.dev)"
//   `cfg.provider == "openai"` w/o provider id  → "openai (custom)"
//   `cfg.provider == "copilot"`                 → "copilot"
std::string format_source_line(const AppConfig& cfg);

// Look up the API key for a provider via env vars in `provider.env` order.
// Returns the (env_name, value) pair on first hit; nullopt if no env var
// is present. On Windows getenv() is case-insensitive to match the shell.
struct EnvKeyHit {
    std::string env_name;
    std::string value;
};
std::optional<EnvKeyHit> lookup_env_key(const ProviderEntry& provider);

// Interactive provider browser. Returns nullptr when the user backs out.
// Output is plain stdout/stdin via terminal_input helpers — TTY only.
const ProviderEntry* run_provider_picker(
    const std::vector<const ProviderEntry*>& providers);

// Interactive model picker for the given provider. Returns the selected model
// id; empty string when the user picked "<Custom model id...>" but typed
// nothing (caller should treat as cancellation).
struct ModelPickerResult {
    bool cancelled = false;
    bool custom = false;                   // user picked the escape hatch
    std::string model_id;                  // either catalog model id or custom typed id
    const ModelEntry* selected = nullptr;  // non-null when picked from catalog
};
ModelPickerResult run_model_picker(const ProviderEntry& provider,
                                   const std::string& default_model_id);

// Drive the catalog-based OpenAI compatible flow inside `configure`. Returns
// true when the user finalised a selection; false when they backed out.
bool configure_openai_via_catalog(AppConfig& cfg);

} // namespace acecode
