#pragma once

#include "llm_provider.hpp"
#include "../config/config.hpp"

namespace acecode {

class OpenAiCompatProvider : public LlmProvider {
public:
    OpenAiCompatProvider(const std::string& base_url,
                         const std::string& api_key,
                         const std::string& model,
                         int stream_timeout_ms = OpenAiConfig::kDefaultStreamTimeoutMs);

    ChatResponse chat(
        const std::vector<ChatMessage>& messages,
        const std::vector<ToolDef>& tools
    ) override;

    void chat_stream(
        const std::vector<ChatMessage>& messages,
        const std::vector<ToolDef>& tools,
        const StreamCallback& callback,
        std::atomic<bool>* abort_flag = nullptr
    ) override;

    std::string name() const override { return "openai"; }
    bool is_authenticated() override { return true; }
    std::string model() const override { return model_; }
    void set_model(const std::string& m) override { model_ = m; }

    // 能力路由上下文(route-attachments-by-capability D5)。create_provider_from_entry
    // 在构造后调用:model_has_vision 来自 entry.capabilities,any_vision_model_available
    // 来自 has_any_runtime_vision_model(config)。daemon 切换模型时必须重新设置。
    // 默认 model_has_vision_=true 是 fail-open —— 未接线时维持旧行为(照发图片),
    // 不会因漏接线把视觉模型的图也剥掉。
    void set_vision_routing(bool model_has_vision, bool any_vision_model_available) {
        model_has_vision_ = model_has_vision;
        any_vision_model_available_ = any_vision_model_available;
    }
    bool model_has_vision() const { return model_has_vision_; }
    bool any_vision_model_available() const { return any_vision_model_available_; }

    // 运行时切换同-provider 的 entry 时,base_url / api_key 可能也变了。
    // 调用方假定在持 provider_mu 锁内调用 —— 不再加内部锁。
    // 对应 openspec/changes/model-profiles 任务 4.4 与 design.md D4。
    void reconfigure(const std::string& base_url,
                     const std::string& api_key,
                     int stream_timeout_ms = OpenAiConfig::kDefaultStreamTimeoutMs) {
        base_url_ = base_url;
        api_key_ = api_key;
        stream_timeout_ms_ = stream_timeout_ms > 0
            ? stream_timeout_ms
            : OpenAiConfig::kDefaultStreamTimeoutMs;
    }

protected:
    // Build the request JSON body (reusable by CopilotProvider)
    nlohmann::json build_request_body(
        const std::vector<ChatMessage>& messages,
        const std::vector<ToolDef>& tools,
        bool stream = false
    ) const;

    // Parse a chat completions response JSON (reusable by CopilotProvider)
    static ChatResponse parse_response(const nlohmann::json& j);

    // Parse SSE stream chunks and call callback. Returns accumulated ChatResponse.
    ChatResponse parse_sse_stream(
        const std::string& url,
        const nlohmann::json& body,
        const std::map<std::string, std::string>& extra_headers,
        const StreamCallback& callback,
        std::atomic<bool>* abort_flag
    );

    std::string base_url_;
    std::string api_key_;
    std::string model_;
    int stream_timeout_ms_ = OpenAiConfig::kDefaultStreamTimeoutMs;
    bool model_has_vision_ = true;             // fail-open,见 set_vision_routing
    bool any_vision_model_available_ = false;
};

// 把一条 ChatMessage 的 content_parts 转成 OpenAI-compatible content payload。
// 这是 image/file 附件路由的唯一收口点(OpenAI + Copilot 共用 build_request_body,
// stream / 非 stream 同源),按模型能力 gate 图片 part。导出到头文件以便单测直接
// 覆盖(route-attachments-by-capability tasks 1.4 / 1.6 / 1.9 / 1.10)。
//   - model_has_vision           : active 模型是否能看图。false 时图片降级为句柄文本。
//   - any_vision_model_available : 系统是否还有可用视觉模型,决定 fallback 文本措辞。
nlohmann::json openai_content_for_message(const ChatMessage& msg,
                                          bool model_has_vision,
                                          bool any_vision_model_available);

} // namespace acecode
