#pragma once

#include "llm_provider.hpp"
#include "../config/config.hpp"

#include <algorithm>
#include <cctype>
#include <map>
#include <string>

namespace acecode {

class AnthropicProvider : public LlmProvider {
public:
    static constexpr const char* kDefaultBaseUrl = "https://api.anthropic.com/v1";
    static constexpr int kDefaultMaxTokens = 4096;

    AnthropicProvider(const std::string& base_url,
                      const std::string& api_key,
                      const std::string& model,
                      int stream_timeout_ms = OpenAiConfig::kDefaultStreamTimeoutMs,
                      std::map<std::string, std::string> request_headers = {});

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

    std::string name() const override { return "anthropic"; }
    bool is_authenticated() override { return !api_key_.empty(); }
    std::string model() const override { return model_; }
    void set_model(const std::string& m) override { model_ = m; }

    static std::string normalize_base_url(std::string value) {
        auto not_space = [](unsigned char c) { return !std::isspace(c); };
        value.erase(value.begin(),
                    std::find_if(value.begin(), value.end(), not_space));
        value.erase(std::find_if(value.rbegin(), value.rend(), not_space).base(),
                    value.end());
        while (!value.empty() && value.back() == '/') value.pop_back();
        return value.empty() ? std::string(kDefaultBaseUrl) : value;
    }

    nlohmann::json build_request_body(
        const std::vector<ChatMessage>& messages,
        const std::vector<ToolDef>& tools,
        bool stream = false
    ) const;

    static ChatResponse parse_response(const nlohmann::json& j);

private:
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
    std::map<std::string, std::string> request_headers_;
    int stream_timeout_ms_ = OpenAiConfig::kDefaultStreamTimeoutMs;
};

} // namespace acecode
