// src/provider/vision_capability.cpp
#include "vision_capability.hpp"

#include "../config/model_provider_registry.hpp"

#include <algorithm>
#include <utility>

namespace acecode {

namespace {
constexpr const char* kVisionCapability = "vision";
} // namespace

bool model_profile_has_vision(const ModelProfile& profile) {
    return std::find(profile.capabilities.begin(),
                     profile.capabilities.end(),
                     kVisionCapability) != profile.capabilities.end();
}

std::vector<ModelProfile> runtime_vision_profiles(const AppConfig& config) {
    std::vector<ModelProfile> out;
    for (auto profile : config.saved_models) {
        if (!is_runtime_model_provider_enabled(profile.provider)) continue;
        if (!model_profile_has_vision(profile)) continue;
        if (profile.provider == "openai" && !profile.stream_timeout_ms.has_value()) {
            profile.stream_timeout_ms = config.openai.stream_timeout_ms;
        }
        out.push_back(std::move(profile));
    }
    return out;
}

bool has_any_runtime_vision_model(const AppConfig& config) {
    for (const auto& profile : config.saved_models) {
        if (!is_runtime_model_provider_enabled(profile.provider)) continue;
        if (model_profile_has_vision(profile)) return true;
    }
    return false;
}

} // namespace acecode
