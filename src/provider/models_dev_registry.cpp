#include "models_dev_registry.hpp"
#include "models_dev_paths.hpp"

#include "../utils/logger.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>

#include <cpr/cpr.h>
#include <cpr/ssl_options.h>

namespace fs = std::filesystem;

namespace acecode {

namespace {

constexpr const char* kModelsDevUrl = "https://models.dev/api.json";

std::mutex& registry_mutex() {
    static std::mutex m;
    return m;
}

std::shared_ptr<const nlohmann::json>& registry_storage() {
    static std::shared_ptr<const nlohmann::json> r =
        std::make_shared<const nlohmann::json>(nlohmann::json::object());
    return r;
}

RegistrySource& source_storage() {
    static RegistrySource s;
    return s;
}

std::string lower(std::string v) {
    std::transform(v.begin(), v.end(), v.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return v;
}

std::optional<nlohmann::json> read_json_file(const fs::path& path) {
    std::error_code ec;
    if (!fs::is_regular_file(path, ec)) return std::nullopt;
    std::ifstream ifs(path);
    if (!ifs.is_open()) return std::nullopt;
    try {
        nlohmann::json j;
        ifs >> j;
        return j;
    } catch (const std::exception& e) {
        LOG_ERROR(std::string("Failed to parse models.dev JSON at ") +
                  path.string() + ": " + e.what());
        return std::nullopt;
    }
}

void install(std::shared_ptr<const nlohmann::json> registry, RegistrySource src) {
    std::lock_guard<std::mutex> lk(registry_mutex());
    registry_storage() = std::move(registry);
    source_storage() = std::move(src);
}

} // namespace

bool validate_registry_schema(const nlohmann::json& registry) {
    if (!registry.is_object() || registry.empty()) return false;
    for (auto it = registry.begin(); it != registry.end(); ++it) {
        const auto& provider = it.value();
        if (!provider.is_object()) continue;
        auto models_it = provider.find("models");
        if (models_it == provider.end()) continue;
        if (models_it->is_object() && !models_it->empty()) return true;
        if (models_it->is_array() && !models_it->empty()) return true;
    }
    return false;
}

const nlohmann::json* find_provider_entry(const nlohmann::json& registry,
                                          const std::string& provider_id) {
    if (!registry.is_object() || provider_id.empty()) return nullptr;
    const std::string needle = lower(provider_id);
    for (auto it = registry.begin(); it != registry.end(); ++it) {
        if (lower(it.key()) == needle && it.value().is_object()) {
            return &it.value();
        }
    }
    return nullptr;
}

void initialize_registry(const AppConfig& cfg, const std::string& argv0_dir) {
    reload_registry_from_disk(cfg, argv0_dir);
}

void reload_registry_from_disk(const AppConfig& cfg, const std::string& argv0_dir) {
    if (cfg.models_dev.user_override_path.has_value() &&
        !cfg.models_dev.user_override_path->empty()) {
        fs::path p(*cfg.models_dev.user_override_path);
        auto parsed = read_json_file(p);
        if (parsed) {
            if (!validate_registry_schema(*parsed)) {
                LOG_ERROR("models.dev user override at " + p.string() +
                          " failed schema validation; treating as empty");
                install(std::make_shared<const nlohmann::json>(nlohmann::json::object()),
                        RegistrySource{RegistrySource::Kind::Empty, p.string(), std::nullopt, std::nullopt});
                return;
            }
            LOG_INFO("Loaded models.dev user override from " + p.string());
            install(std::make_shared<const nlohmann::json>(std::move(*parsed)),
                    RegistrySource{RegistrySource::Kind::UserOverride, p.string(), std::nullopt, std::nullopt});
            return;
        }
        LOG_WARN("models.dev user_override_path '" + p.string() +
                 "' is missing or unreadable; falling back to bundled snapshot");
    }

    auto seed = find_models_dev_dir(argv0_dir);
    if (seed) {
        fs::path api_path = fs::path(*seed) / "api.json";
        auto parsed = read_json_file(api_path);
        if (parsed) {
            std::optional<nlohmann::json> manifest =
                read_json_file(fs::path(*seed) / "MANIFEST.json");

            if (!validate_registry_schema(*parsed)) {
                LOG_ERROR("Bundled models.dev registry at " + api_path.string() +
                          " failed schema validation; treating as empty");
                install(std::make_shared<const nlohmann::json>(nlohmann::json::object()),
                        RegistrySource{RegistrySource::Kind::Empty, api_path.string(),
                                       std::move(manifest), seed});
                return;
            }
            LOG_INFO("Loaded bundled models.dev registry from " + api_path.string());
            install(std::make_shared<const nlohmann::json>(std::move(*parsed)),
                    RegistrySource{RegistrySource::Kind::Bundled, api_path.string(),
                                   std::move(manifest), seed});
            return;
        }
    }

    LOG_WARN("models.dev registry not available (no user override, no bundled snapshot)");
    install(std::make_shared<const nlohmann::json>(nlohmann::json::object()),
            RegistrySource{RegistrySource::Kind::Empty, "", std::nullopt, std::nullopt});
}

bool refresh_registry_from_network() {
    cpr::Response r = cpr::Get(
        cpr::Url{kModelsDevUrl},
        cpr::Ssl(cpr::ssl::NoRevoke{true}),
        cpr::Timeout{20000}
    );
    if (r.status_code != 200) {
        LOG_INFO("models.dev network refresh failed (status=" +
                 std::to_string(r.status_code) + "), keeping current snapshot");
        return false;
    }
    try {
        auto parsed = nlohmann::json::parse(r.text);
        if (!validate_registry_schema(parsed)) {
            LOG_INFO("models.dev network response failed schema validation; keeping current snapshot");
            return false;
        }
        LOG_INFO("models.dev network refresh succeeded");
        install(std::make_shared<const nlohmann::json>(std::move(parsed)),
                RegistrySource{RegistrySource::Kind::Network, kModelsDevUrl, std::nullopt, std::nullopt});
        return true;
    } catch (const std::exception& e) {
        LOG_INFO(std::string("models.dev network refresh parse error: ") + e.what());
        return false;
    }
}

std::shared_ptr<const nlohmann::json> current_registry() {
    std::lock_guard<std::mutex> lk(registry_mutex());
    return registry_storage();
}

const RegistrySource& current_registry_source() {
    return source_storage();
}

} // namespace acecode
