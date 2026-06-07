#include "model_provider_registry.hpp"

namespace acecode {

bool is_known_model_provider(const std::string& provider) {
    return provider == "openai" || provider == "copilot" || provider == "codex";
}

bool is_runtime_model_provider_enabled(const std::string& provider) {
    return provider == "openai" || provider == "copilot";
}

const char* disabled_model_provider_reason(const std::string& provider) {
    if (provider == "codex") {
        return "Codex provider is disabled until ACECode can own Codex tool "
               "execution and file-write tracking";
    }
    return "";
}

} // namespace acecode
