#include "session_auto_title.hpp"

#include "session_title_generator.hpp"
#include "../provider/copilot_provider.hpp"
#include "../provider/cwd_model_override.hpp"
#include "../provider/model_resolver.hpp"
#include "../provider/provider_factory.hpp"
#include "../utils/logger.hpp"

#include <cctype>
#include <sstream>
#include <utility>

namespace acecode {

namespace {

std::string trim_ascii(std::string s) {
    size_t b = 0, e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return s.substr(b, e - b);
}

const ModelProfile* find_profile_by_name(const AppConfig& cfg,
                                          const std::string& name) {
    if (name.empty()) return nullptr;
    for (const auto& entry : cfg.saved_models) {
        if (entry.name == name) return &entry;
    }
    return nullptr;
}

std::optional<ModelProfile> explicit_profile(const AppConfig& cfg,
                                             const std::string& name) {
    if (const auto* entry = find_profile_by_name(cfg, name)) {
        ModelProfile profile = *entry;
        if (profile.provider == "openai" && !profile.stream_timeout_ms.has_value()) {
            profile.stream_timeout_ms = cfg.openai.stream_timeout_ms;
        }
        return profile;
    }
    return std::nullopt;
}

void clamp_title_profile_timeout(ModelProfile& profile, const AppConfig& cfg) {
    if (profile.provider != "openai") return;
    const int timeout = cfg.session_title.timeout_ms;
    if (!profile.stream_timeout_ms.has_value() || *profile.stream_timeout_ms > timeout) {
        profile.stream_timeout_ms = timeout;
    }
}

} // namespace

std::string visible_auto_title_input(const UserInput& input) {
    std::string text = input.display_text.empty() ? input.text : input.display_text;
    text = trim_ascii(std::move(text));
    if (!text.empty()) return text;
    if (!input.content_parts.is_array()) return {};
    std::ostringstream out;
    for (const auto& part : input.content_parts) {
        if (!part.is_object()) continue;
        const std::string type = part.value("type", std::string{});
        if (type == "text" && part.contains("text") && part["text"].is_string()) {
            const std::string piece = trim_ascii(part["text"].get<std::string>());
            if (!piece.empty()) {
                if (out.tellp() > 0) out << "\n";
                out << piece;
            }
        }
    }
    return trim_ascii(out.str());
}

std::optional<ModelProfile> resolve_auto_title_profile(
    const AppConfig& cfg,
    const std::string& session_model_name,
    const std::string& cwd) {
    if (!cfg.session_title.model_name.empty()) {
        if (auto profile = explicit_profile(cfg, cfg.session_title.model_name)) {
            return profile;
        }
        LOG_WARN("[auto_title] session_title.model_name '" +
                 cfg.session_title.model_name +
                 "' not found; falling back to current session model");
    }
    if (!session_model_name.empty()) {
        if (auto profile = explicit_profile(cfg, session_model_name)) {
            return profile;
        }
    }
    std::optional<std::string> cwd_override;
    if (!cwd.empty()) {
        cwd_override = load_cwd_model_override(cwd);
    }
    return resolve_effective_model(cfg, cwd_override, std::nullopt);
}

std::shared_ptr<LlmProvider> create_auto_title_provider(ModelProfile profile,
                                                        const AppConfig& cfg) {
    clamp_title_profile_timeout(profile, cfg);
    auto provider = create_provider_from_entry(profile, &cfg);
    if (auto copilot = std::dynamic_pointer_cast<CopilotProvider>(provider)) {
        copilot->try_silent_auth();
    }
    return provider;
}

std::optional<std::string> generate_auto_session_title(
    LlmProvider& provider,
    const std::string& visible_text,
    const AppConfig& cfg) {
    return generate_session_title(
        provider,
        visible_text,
        cfg.session_title.max_input_bytes);
}

} // namespace acecode
