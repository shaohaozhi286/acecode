#pragma once

#include "llm_provider.hpp"

#include <string>

namespace acecode {

class CodexProvider : public LlmProvider {
public:
    explicit CodexProvider(std::string model);

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

    std::string name() const override { return "codex"; }
    bool is_authenticated() override;
    bool authenticate() override { return is_authenticated(); }

    std::string model() const override { return model_; }
    void set_model(const std::string& m) override { model_ = m; }

private:
    std::string model_;
};

} // namespace acecode
