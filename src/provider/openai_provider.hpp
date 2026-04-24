#pragma once

#include "llm_provider.hpp"
#include "../config/config.hpp"

namespace acecode {

class OpenAiCompatProvider : public LlmProvider {
public:
    OpenAiCompatProvider(const std::string& base_url,
                         const std::string& api_key,
                         const std::string& model);

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

    // 运行时切换同-provider 的 entry 时,base_url / api_key 可能也变了。
    // 调用方假定在持 provider_mu 锁内调用 —— 不再加内部锁。
    // 对应 openspec/changes/model-profiles 任务 4.4 与 design.md D4。
    void reconfigure(const std::string& base_url, const std::string& api_key) {
        base_url_ = base_url;
        api_key_ = api_key;
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
};

} // namespace acecode
