#pragma once

#include "../config/config.hpp"

#include <memory>
#include <optional>
#include <string>

#include <nlohmann/json.hpp>

namespace acecode {

// Where the in-memory registry came from. `manifest` is populated only when the
// snapshot came from `<seed>/api.json` and `<seed>/MANIFEST.json` was readable.
struct RegistrySource {
    enum class Kind { Empty, Bundled, UserOverride, Network };
    Kind kind = Kind::Empty;
    std::string path_or_url;            // file path for Bundled/UserOverride, URL for Network
    std::optional<nlohmann::json> manifest;
    std::optional<std::string> seed_dir; // populated for Bundled
};

// Load the registry once according to AppConfig. Order: user_override_path →
// bundled api.json (via find_models_dev_dir) → empty. Network fetch is NEVER
// performed by this function — call refresh_registry_from_network() explicitly.
//
// Performs minimal validation: top-level must be a JSON object containing at
// least one provider that has a non-empty `models` field. Validation failure
// logs ERROR but still returns a non-null registry pointer wrapping an empty
// object so call sites can keep working.
//
// `argv0_dir` is forwarded to find_models_dev_dir(); empty disables that lookup.
void initialize_registry(const AppConfig& cfg, const std::string& argv0_dir);

// Force re-load from disk using the same logic as initialize_registry().
void reload_registry_from_disk(const AppConfig& cfg, const std::string& argv0_dir);

// Refresh from https://models.dev/api.json. Result lives in memory only — never
// written to disk. Returns true on success (registry updated), false on any
// failure (registry untouched).
bool refresh_registry_from_network();

// Current shared registry. Always non-null after initialize_registry() ran.
std::shared_ptr<const nlohmann::json> current_registry();

// Provenance of the currently-loaded registry.
const RegistrySource& current_registry_source();

// Helper for tests / `/models lookup` — find a provider object in the supplied
// registry JSON by id (case-insensitive match against top-level keys). Returns
// nullptr when not found.
const nlohmann::json* find_provider_entry(const nlohmann::json& registry,
                                          const std::string& provider_id);

// Apply minimal schema validation. Returns true when the JSON is acceptable
// (top-level object, at least one provider with a non-empty `models` field).
bool validate_registry_schema(const nlohmann::json& registry);

} // namespace acecode
