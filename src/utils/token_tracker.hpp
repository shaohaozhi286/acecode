#pragma once

#include "../provider/llm_provider.hpp"
#include <string>
#include <mutex>

namespace acecode {

class TokenTracker {
public:
    struct Snapshot {
        TokenUsage last;
        TokenUsage session;
    };

    // Record usage from a server response.
    // Updates both "last" (current context) and "session" (cumulative) counters.
    void record(const TokenUsage& usage);

    // Estimate and record tokens from character count (fallback when no server usage)
    void record_estimate(int char_count);

    // --- "Last" counters (most recent API call = current context occupancy) ---
    int last_prompt_tokens() const;
    int last_completion_tokens() const;

    // --- Session cumulative counters ---
    int prompt_tokens() const;
    int completion_tokens() const;
    int total_tokens() const;
    int cache_read_tokens() const;
    int cache_write_tokens() const;

    Snapshot snapshot() const;
    void restore(const Snapshot& snapshot);
    void restore(const TokenUsage& last, const TokenUsage& session);

    // Reset all counters
    void reset();

    // Format token count for display (e.g., "1.2k", "45.3k")
    static std::string format_tokens(int count);

    // Format status bar string: "8.0k/128k"
    // Shows current context occupancy (last_prompt_tokens / context_window)
    std::string format_status(int context_window) const;

private:
    mutable std::mutex mu_;

    // Most recent API call (current context window occupancy)
    int last_prompt_tokens_ = 0;
    int last_completion_tokens_ = 0;
    int last_total_tokens_ = 0;
    int last_cache_read_tokens_ = 0;
    int last_cache_write_tokens_ = 0;
    int last_reasoning_tokens_ = 0;
    bool last_has_data_ = false;

    // Session cumulative
    int session_prompt_tokens_ = 0;
    int session_completion_tokens_ = 0;
    int session_total_tokens_ = 0;
    int session_cache_read_tokens_ = 0;
    int session_cache_write_tokens_ = 0;
    int session_reasoning_tokens_ = 0;
    bool session_has_data_ = false;
};

} // namespace acecode
