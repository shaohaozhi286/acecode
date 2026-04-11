#pragma once

#include "openai_provider.hpp"
#include "../auth/github_auth.hpp"

#include <chrono>

namespace acecode {

class CopilotProvider : public OpenAiCompatProvider {
public:
    explicit CopilotProvider(const std::string& model);

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

    std::string name() const override { return "copilot"; }
    bool is_authenticated() override;
    bool authenticate() override;

    // Access device code info for TUI display during auth
    const DeviceCodeResponse& device_code_info() const { return device_code_; }

    // Non-interactive auth: try loading saved token and exchanging for copilot token
    bool try_silent_auth();

    // Interactive auth: run device flow. status_callback is called with status updates.
    bool run_device_flow(std::function<void(const std::string&)> status_callback = nullptr);

private:
    bool ensure_copilot_token();

    std::string github_token_;
    CopilotToken copilot_token_;
    DeviceCodeResponse device_code_;
};

} // namespace acecode
