#pragma once

// Stub LlmProvider for agent-loop testing:
// - Each call to chat_stream pops the next scripted response off `responses_`
//   and replays it as a sequence of StreamEvents.
// - A response is either:
//     (a) pure text            → Delta + Done
//     (b) pure tool call list  → ToolCall* + Done
//     (c) text + tool calls    → Delta + ToolCall* + Done
// - If the script runs out, the provider returns a harmless empty Delta so the
//   agent loop's empty-turn branch fires predictably. (Tests that want to
//   exercise "model never stops" simply queue many empty responses.)
//
// Thread-safe enough for the agent loop to call chat_stream on its worker
// thread while the test main thread queues responses before submit(): we lock
// a mutex on every access.

#include "provider/llm_provider.hpp"

#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace acecode_test {

struct ScriptedResponse {
    std::string text;                         // may be empty
    std::vector<acecode::ToolCall> tool_calls; // may be empty
};

class StubLlmProvider : public acecode::LlmProvider {
public:
    // Queue a response for a future turn. Thread-safe.
    void push_response(ScriptedResponse r) {
        std::lock_guard<std::mutex> lk(mu_);
        responses_.push_back(std::move(r));
    }

    void push_text(std::string s) {
        push_response({std::move(s), {}});
    }

    void push_tool_call(std::string tool_name, std::string args_json,
                        std::string call_id = "call-auto") {
        ScriptedResponse r;
        acecode::ToolCall tc;
        tc.id = std::move(call_id);
        tc.function_name = std::move(tool_name);
        tc.function_arguments = std::move(args_json);
        r.tool_calls.push_back(std::move(tc));
        push_response(std::move(r));
    }

    int turn_count() const {
        std::lock_guard<std::mutex> lk(mu_);
        return turn_count_;
    }

    // Simulate LLM latency by polling abort_flag inside chat_stream for ~ms
    // before emitting the scripted response. 0 (default) = no latency.
    // Abort-tests use this to guarantee a window where abort() can land before
    // the loop advances to its cap-check.
    void set_latency_ms(int ms) {
        std::lock_guard<std::mutex> lk(mu_);
        latency_ms_ = ms;
    }

    // LlmProvider interface -------------------------------------------------

    acecode::ChatResponse chat(
        const std::vector<acecode::ChatMessage>& /*messages*/,
        const std::vector<acecode::ToolDef>& /*tools*/) override {
        // Non-streaming path unused by AgentLoop; return an empty text.
        acecode::ChatResponse r;
        r.finish_reason = "stop";
        return r;
    }

    void chat_stream(
        const std::vector<acecode::ChatMessage>& /*messages*/,
        const std::vector<acecode::ToolDef>& /*tools*/,
        const acecode::StreamCallback& callback,
        std::atomic<bool>* abort_flag = nullptr) override {
        ScriptedResponse r;
        int latency_ms;
        {
            std::lock_guard<std::mutex> lk(mu_);
            if (!responses_.empty()) {
                r = std::move(responses_.front());
                responses_.erase(responses_.begin());
            }
            ++turn_count_;
            latency_ms = latency_ms_;
        }

        // Optional latency: poll abort_flag so aborts land deterministically.
        for (int i = 0; i < latency_ms / 5; ++i) {
            if (abort_flag && abort_flag->load()) {
                acecode::StreamEvent done_evt;
                done_evt.type = acecode::StreamEventType::Done;
                callback(done_evt);
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }

        if (!r.text.empty()) {
            acecode::StreamEvent evt;
            evt.type = acecode::StreamEventType::Delta;
            evt.content = r.text;
            callback(evt);
        }
        for (auto& tc : r.tool_calls) {
            acecode::StreamEvent evt;
            evt.type = acecode::StreamEventType::ToolCall;
            evt.tool_call = std::move(tc);
            callback(evt);
        }
        acecode::StreamEvent done_evt;
        done_evt.type = acecode::StreamEventType::Done;
        callback(done_evt);
    }

    std::string name() const override { return "stub"; }
    bool is_authenticated() override { return true; }
    std::string model() const override { return "stub-1"; }
    void set_model(const std::string& /*m*/) override {}

private:
    mutable std::mutex mu_;
    std::vector<ScriptedResponse> responses_;
    int turn_count_ = 0;
    int latency_ms_ = 0;
};

} // namespace acecode_test
