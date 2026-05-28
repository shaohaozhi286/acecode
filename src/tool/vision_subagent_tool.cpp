#include "vision_subagent_tool.hpp"

#include "../config/model_provider_registry.hpp"
#include "../provider/copilot_provider.hpp"
#include "../provider/provider_factory.hpp"
#include "../session/attachment_store.hpp"
#include "../session/session_manager.hpp"
#include "../utils/encoding.hpp"
#include "../utils/logger.hpp"

#include <algorithm>
#include <atomic>
#include <random>
#include <sstream>
#include <utility>

namespace acecode {

namespace {

constexpr const char* kVisionCapability = "vision";

ToolResult error_result(const std::string& code, const std::string& message) {
    nlohmann::json out = {
        {"ok", false},
        {"error", code},
        {"message", message},
    };
    return ToolResult{out.dump(2), false};
}

bool has_capability(const ModelProfile& profile, const std::string& capability) {
    return std::find(profile.capabilities.begin(),
                     profile.capabilities.end(),
                     capability) != profile.capabilities.end();
}

bool is_image_attachment(const AttachmentRecord& record) {
    return record.kind == "image" || record.mime_type.rfind("image/", 0) == 0;
}

std::optional<AttachmentRecord> record_from_part(const nlohmann::json& part) {
    if (!part.is_object()) return std::nullopt;
    if (part.value("type", std::string{}) != "image") return std::nullopt;
    if (!part.contains("attachment")) return std::nullopt;
    auto record = attachment_from_json(part["attachment"]);
    if (!record.has_value() || !is_image_attachment(*record)) return std::nullopt;
    return record;
}

std::optional<AttachmentRecord> latest_image_from_messages(
    const std::vector<ChatMessage>& messages,
    const std::string& attachment_id) {
    for (auto mit = messages.rbegin(); mit != messages.rend(); ++mit) {
        if (mit->content_parts.is_null() || !mit->content_parts.is_array()) {
            continue;
        }
        for (auto pit = mit->content_parts.rbegin();
             pit != mit->content_parts.rend();
             ++pit) {
            auto record = record_from_part(*pit);
            if (!record.has_value()) continue;
            if (!attachment_id.empty() && record->id != attachment_id) continue;
            return record;
        }
    }
    return std::nullopt;
}

std::optional<AttachmentRecord> resolve_attachment(
    const nlohmann::json& args,
    const ToolContext& ctx,
    std::string* error_code,
    std::string* error_message) {
    if (args.contains("attachment") && args["attachment"].is_object()) {
        auto record = attachment_from_json(args["attachment"]);
        if (!record.has_value() || !is_image_attachment(*record)) {
            *error_code = "INVALID_ATTACHMENT";
            *error_message = "attachment must be valid image attachment metadata";
            return std::nullopt;
        }
        return record;
    }

    std::string attachment_id;
    if (args.contains("attachment_id") && args["attachment_id"].is_string()) {
        attachment_id = args["attachment_id"].get<std::string>();
    }

    if (!ctx.session_manager) {
        *error_code = "NO_ACTIVE_SESSION";
        *error_message =
            "vision_analyze needs an active session or explicit attachment metadata";
        return std::nullopt;
    }

    auto messages = ctx.session_manager->load_active_messages();
    auto record = latest_image_from_messages(messages, attachment_id);
    if (!record.has_value()) {
        *error_code = attachment_id.empty() ? "NO_IMAGE_ATTACHMENT" : "IMAGE_ATTACHMENT_NOT_FOUND";
        *error_message = attachment_id.empty()
            ? "No image attachment found in the active session"
            : "No image attachment with id '" + attachment_id + "' found in the active session";
        return std::nullopt;
    }
    return record;
}

std::vector<ModelProfile> vision_profiles(const AppConfig& config) {
    std::vector<ModelProfile> out;
    for (auto profile : config.saved_models) {
        if (!is_runtime_model_provider_enabled(profile.provider)) continue;
        if (!has_capability(profile, kVisionCapability)) continue;
        if (profile.provider == "openai" && !profile.stream_timeout_ms.has_value()) {
            profile.stream_timeout_ms = config.openai.stream_timeout_ms;
        }
        out.push_back(std::move(profile));
    }
    return out;
}

std::optional<ModelProfile> select_vision_profile(
    const AppConfig& config,
    const std::string& model_name,
    const VisionSubagentToolOptions::IndexChooser& choose_index,
    std::string* error_code,
    std::string* error_message) {
    auto candidates = vision_profiles(config);

    if (!model_name.empty()) {
        auto it = std::find_if(candidates.begin(), candidates.end(),
            [&](const ModelProfile& p) { return p.name == model_name; });
        if (it == candidates.end()) {
            *error_code = "VISION_MODEL_NOT_FOUND";
            *error_message =
                "Saved model '" + model_name + "' is not tagged with capability 'vision'";
            return std::nullopt;
        }
        return *it;
    }

    if (candidates.empty()) {
        *error_code = "NO_VISION_MODEL";
        *error_message =
            "No saved model is tagged with capability 'vision'. Configure a saved model and select the vision capability.";
        return std::nullopt;
    }

    std::size_t index = 0;
    if (candidates.size() > 1) {
        if (choose_index) {
            index = choose_index(candidates.size()) % candidates.size();
        } else {
            std::random_device rd;
            std::mt19937 gen(rd());
            std::uniform_int_distribution<std::size_t> dist(0, candidates.size() - 1);
            index = dist(gen);
        }
    }
    return candidates[index];
}

std::shared_ptr<LlmProvider> default_provider_factory(const ModelProfile& profile) {
    auto provider = create_provider_from_entry(profile);
    if (provider && provider->name() == "copilot") {
        if (auto copilot = std::dynamic_pointer_cast<CopilotProvider>(provider)) {
            if (!copilot->try_silent_auth()) {
                LOG_WARN("[vision_analyze] Copilot silent auth failed for model '" +
                         profile.name + "'");
            }
        }
    }
    return provider;
}

ToolResult execute_vision_analyze(
    const std::string& args_json,
    const ToolContext& ctx,
    const AppConfig* config,
    VisionSubagentToolOptions options) {
    if (!config) {
        return error_result("CONFIG_UNAVAILABLE", "configuration unavailable");
    }

    nlohmann::json args;
    try {
        args = args_json.empty() ? nlohmann::json::object()
                                 : nlohmann::json::parse(args_json);
    } catch (const std::exception& e) {
        return error_result("BAD_REQUEST", std::string("bad json: ") + e.what());
    }
    if (!args.is_object()) {
        return error_result("BAD_REQUEST", "arguments must be a JSON object");
    }

    const std::string prompt = args.value("prompt", std::string{});
    if (prompt.empty()) {
        return error_result("MISSING_PROMPT", "prompt is required");
    }
    const std::string model_name = args.value("model_name", std::string{});

    std::string error_code;
    std::string error_message;
    auto attachment = resolve_attachment(args, ctx, &error_code, &error_message);
    if (!attachment.has_value()) {
        return error_result(error_code, error_message);
    }

    auto profile = select_vision_profile(
        *config, model_name, options.choose_index, &error_code, &error_message);
    if (!profile.has_value()) {
        return error_result(error_code, error_message);
    }

    auto factory = options.provider_factory
        ? options.provider_factory
        : VisionSubagentToolOptions::ProviderFactory(default_provider_factory);
    auto provider = factory(*profile);
    if (!provider) {
        return error_result("PROVIDER_UNAVAILABLE",
                            "failed to create provider for saved model '" + profile->name + "'");
    }

    ChatMessage user;
    user.role = "user";
    user.content = prompt;
    user.content_parts = nlohmann::json::array({
        nlohmann::json{{"type", "text"}, {"text", prompt}},
        nlohmann::json{{"type", "image"}, {"attachment", attachment_to_json(*attachment)}},
    });

    ChatResponse response;
    try {
        response = provider->chat({user}, {});
    } catch (const std::exception& e) {
        return error_result("PROVIDER_ERROR", e.what());
    }

    nlohmann::json out = {
        {"ok", true},
        {"model_name", profile->name},
        {"provider", profile->provider},
        {"model", profile->model},
        {"attachment_id", attachment->id},
        {"content", ensure_utf8(response.content)},
        {"finish_reason", response.finish_reason},
    };
    if (!response.reasoning_content.empty()) {
        out["reasoning_content"] = ensure_utf8(response.reasoning_content);
    }
    if (response.usage.has_data) {
        out["usage"] = {
            {"prompt_tokens", response.usage.prompt_tokens},
            {"completion_tokens", response.usage.completion_tokens},
            {"total_tokens", response.usage.total_tokens},
        };
    }
    return ToolResult{out.dump(2), true};
}

} // namespace

ToolImpl create_vision_analyze_tool(
    const AppConfig& config,
    VisionSubagentToolOptions options) {
    ToolDef def;
    def.name = "vision_analyze";
    def.description =
        "Analyze an image by making an internal one-shot call to a saved model "
        "tagged with the 'vision' capability. Use this when the active model "
        "cannot reliably inspect images. The call is not a resumable or visible "
        "session.";
    def.parameters = nlohmann::json({
        {"type", "object"},
        {"properties", {
            {"prompt", {
                {"type", "string"},
                {"description", "Question or task for the vision-capable model"}
            }},
            {"attachment_id", {
                {"type", "string"},
                {"description", "Optional image attachment id. If omitted, the latest image attachment in the active session is used."}
            }},
            {"attachment", {
                {"type", "object"},
                {"description", "Optional full image attachment metadata object for direct callers."}
            }},
            {"model_name", {
                {"type", "string"},
                {"description", "Optional saved model name. It must be tagged with the vision capability."}
            }}
        }},
        {"required", nlohmann::json::array({"prompt"})}
    });

    const AppConfig* config_ptr = &config;
    auto exec = [config_ptr, options = std::move(options)](
        const std::string& args_json,
        const ToolContext& ctx) -> ToolResult {
        return execute_vision_analyze(args_json, ctx, config_ptr, options);
    };
    return ToolImpl{def, std::move(exec), /*is_read_only=*/true};
}

} // namespace acecode
