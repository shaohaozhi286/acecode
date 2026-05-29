#include "vision_subagent_tool.hpp"

#include "../config/model_provider_registry.hpp"
#include "../provider/copilot_provider.hpp"
#include "../provider/provider_factory.hpp"
#include "../provider/vision_capability.hpp"
#include "../session/attachment_store.hpp"
#include "../session/session_manager.hpp"
#include "../session/session_storage.hpp"
#include "../utils/encoding.hpp"
#include "../utils/logger.hpp"
#include "../utils/utf8_path.hpp"

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <random>
#include <sstream>
#include <utility>

namespace acecode {

namespace {

ToolResult error_result(const std::string& code, const std::string& message) {
    nlohmann::json out = {
        {"ok", false},
        {"error", code},
        {"message", message},
    };
    return ToolResult{out.dump(2), false};
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

// 把一个本地图片路径物化成 active session 的图片附件(tasks 2.2 / 2.3 / 2.4)。
// 相对路径基于 ToolContext.cwd 解析,绝对路径允许指向 workspace 外。校验顺序为先
// stat(存在性 / 普通文件 / 大小),再读字节,避免把超大文件整个读进内存。
std::optional<AttachmentRecord> materialize_image_path(
    const std::string& image_path,
    const ToolContext& ctx,
    std::string* error_code,
    std::string* error_message) {
    namespace fs = std::filesystem;

    if (!ctx.session_manager) {
        *error_code = "NO_ACTIVE_SESSION";
        *error_message =
            "image_path materialization needs an active session to store the attachment";
        return std::nullopt;
    }

    fs::path resolved = path_from_utf8(image_path);
    if (resolved.is_relative()) {
        resolved = path_from_utf8(ctx.cwd) / resolved;
    }

    std::error_code ec;
    const auto status = fs::status(resolved, ec);
    if (ec || !fs::exists(status)) {
        *error_code = "IMAGE_PATH_NOT_FOUND";
        *error_message = "image_path does not exist: " + image_path;
        return std::nullopt;
    }
    if (!fs::is_regular_file(status)) {
        *error_code = "IMAGE_PATH_NOT_FILE";
        *error_message = "image_path must reference a regular file: " + image_path;
        return std::nullopt;
    }

    const auto size = fs::file_size(resolved, ec);
    if (ec) {
        *error_code = "IMAGE_PATH_NOT_FOUND";
        *error_message = "failed to stat image_path: " + image_path;
        return std::nullopt;
    }
    if (size > kMaxAttachmentBytes) {
        *error_code = "IMAGE_TOO_LARGE";
        *error_message = "image_path exceeds the attachment size limit";
        return std::nullopt;
    }

    const std::string filename = path_to_utf8(resolved.filename());
    const std::string mime = attachment_mime_for_name(filename, std::string{});
    if (attachment_kind_for_mime(mime, filename) != "image") {
        *error_code = "NOT_IMAGE";
        *error_message =
            "image_path must reference an image file (png/jpeg/gif/webp/bmp)";
        return std::nullopt;
    }

    std::ifstream ifs(resolved, std::ios::binary);
    if (!ifs.is_open()) {
        *error_code = "IMAGE_READ_FAILED";
        *error_message = "failed to open image_path: " + image_path;
        return std::nullopt;
    }
    std::string bytes((std::istreambuf_iterator<char>(ifs)),
                      std::istreambuf_iterator<char>());
    if (ifs.bad()) {
        *error_code = "IMAGE_READ_FAILED";
        *error_message = "failed to read image_path: " + image_path;
        return std::nullopt;
    }

    const std::string project_dir = SessionStorage::get_project_dir(ctx.cwd);
    const std::string session_id = ctx.session_manager->ensure_active_session_id();
    std::string save_error;
    auto record = save_attachment(project_dir, session_id, filename, mime, bytes, &save_error);
    if (!record.has_value()) {
        *error_code = "SAVE_FAILED";
        *error_message = save_error.empty() ? "failed to save image attachment" : save_error;
        return std::nullopt;
    }
    // image_path 物化会在 session attachment 目录留下一份独立 blob;vision 调用本身
    // 隐藏不落 transcript,这条 LOG 便于排查体积增长(tasks 2.7 / design 风险)。
    LOG_INFO("[vision_analyze] materialized image_path attachment id=" + record->id +
             " session=" + session_id +
             " bytes=" + std::to_string(record->size_bytes) +
             " (not attached to a visible message)");
    return record;
}

std::optional<AttachmentRecord> resolve_attachment(
    const nlohmann::json& args,
    const ToolContext& ctx,
    std::string* error_code,
    std::string* error_message) {
    if (args.contains("image_path") && args["image_path"].is_string() &&
        !args["image_path"].get<std::string>().empty()) {
        return materialize_image_path(
            args["image_path"].get<std::string>(), ctx, error_code, error_message);
    }

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

std::optional<ModelProfile> select_vision_profile(
    const AppConfig& config,
    const std::string& model_name,
    const VisionSubagentToolOptions::IndexChooser& choose_index,
    std::string* error_code,
    std::string* error_message) {
    // 共享 helper:与序列化层 fallback 的"是否存在可用视觉模型"判定同口径(D5 / 1.8)。
    auto candidates = runtime_vision_profiles(config);

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
    // 先选视觉模型,再解析/物化附件:无可用视觉模型时直接拒绝,避免 image_path
    // 先被物化成孤儿附件再失败(tasks 2.6 / design 风险)。
    auto profile = select_vision_profile(
        *config, model_name, options.choose_index, &error_code, &error_message);
    if (!profile.has_value()) {
        return error_result(error_code, error_message);
    }

    auto attachment = resolve_attachment(args, ctx, &error_code, &error_message);
    if (!attachment.has_value()) {
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
            {"image_path", {
                {"type", "string"},
                {"description", "Optional local image file path to analyze. Relative paths resolve against the session working directory; absolute paths may point outside the workspace. The file is read, validated as an image, and stored as a session attachment before being sent to the vision model. Takes precedence over attachment_id / latest-image fallback."}
            }},
            {"attachment_id", {
                {"type", "string"},
                {"description", "Optional image attachment id. If omitted (and no image_path given), the latest image attachment in the active session is used."}
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
