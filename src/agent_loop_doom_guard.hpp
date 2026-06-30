#pragma once

#include "provider/llm_provider.hpp"
#include "tool/tool_executor.hpp"

#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace acecode {

class AgentLoopDoomGuard {
public:
    enum class Operation {
        Unknown,
        Read,
        Search,
        Verify,
        Write,
    };

    enum class ResultClass {
        Useful,
        Empty,
        Error,
        Denied,
        NotFound,
        Timeout,
        Unchanged,
        Guarded,
    };

    // Called at the beginning of each provider turn. Cooldowns are intentionally
    // turn-based so several tool calls in the same assistant response share the
    // same guard state, while a later model turn can recover after using other tools.
    void begin_model_turn();
    void reset();

    // Returns a synthetic tool result when the call should be skipped. A null
    // result means the real tool should run.
    std::optional<ToolResult> maybe_guard(const ToolCall& call);

    // Record the result of either a real or synthetic tool call.
    void record_result(const ToolCall& call, const ToolResult& result);

private:
    struct CallKey {
        std::string tool;
        std::string exact;
        std::string semantic;
        Operation operation = Operation::Unknown;
        std::string target;
    };

    struct Attempt {
        CallKey key;
        ResultClass result = ResultClass::Useful;
        bool low_signal = false;
    };

    CallKey build_key(const ToolCall& call) const;
    ResultClass classify_result(const ToolResult& result) const;
    bool is_low_signal(ResultClass result) const;
    ToolResult make_cached_read_result(const CallKey& key) const;
    ToolResult make_synthetic_result(const CallKey& key,
                                     const std::string& reason,
                                     bool cooldown_active) const;
    int exact_result_count(const CallKey& key, ResultClass result) const;
    int low_signal_exact_count(const CallKey& key) const;
    int low_signal_semantic_count(const CallKey& key) const;
    void start_cooldown(const std::string& tool, int turns);
    bool cooldown_active(const std::string& tool) const;

    std::vector<Attempt> attempts_;
    std::unordered_map<std::string, int> cooldown_turns_;
};

} // namespace acecode
