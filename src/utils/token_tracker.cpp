#include "token_tracker.hpp"
#include <sstream>
#include <iomanip>

namespace acecode {

void TokenTracker::record(const TokenUsage& usage) {
    std::lock_guard<std::mutex> lk(mu_);
    // Update "last" counters (current context occupancy)
    last_prompt_tokens_ = usage.prompt_tokens;
    last_completion_tokens_ = usage.completion_tokens;

    // Accumulate session totals
    session_prompt_tokens_ += usage.prompt_tokens;
    session_completion_tokens_ += usage.completion_tokens;
    session_total_tokens_ += usage.total_tokens;
    session_cache_read_tokens_ += usage.cache_read_tokens;
    session_cache_write_tokens_ += usage.cache_write_tokens;
}

void TokenTracker::record_estimate(int char_count) {
    std::lock_guard<std::mutex> lk(mu_);
    int estimated = char_count / 4;
    // For estimates, update last_prompt as well (best guess for current context)
    last_prompt_tokens_ = estimated;
    session_total_tokens_ += estimated;
}

int TokenTracker::last_prompt_tokens() const {
    std::lock_guard<std::mutex> lk(mu_);
    return last_prompt_tokens_;
}

int TokenTracker::last_completion_tokens() const {
    std::lock_guard<std::mutex> lk(mu_);
    return last_completion_tokens_;
}

int TokenTracker::prompt_tokens() const {
    std::lock_guard<std::mutex> lk(mu_);
    return session_prompt_tokens_;
}

int TokenTracker::completion_tokens() const {
    std::lock_guard<std::mutex> lk(mu_);
    return session_completion_tokens_;
}

int TokenTracker::total_tokens() const {
    std::lock_guard<std::mutex> lk(mu_);
    return session_total_tokens_;
}

int TokenTracker::cache_read_tokens() const {
    std::lock_guard<std::mutex> lk(mu_);
    return session_cache_read_tokens_;
}

int TokenTracker::cache_write_tokens() const {
    std::lock_guard<std::mutex> lk(mu_);
    return session_cache_write_tokens_;
}

void TokenTracker::reset() {
    std::lock_guard<std::mutex> lk(mu_);
    last_prompt_tokens_ = 0;
    last_completion_tokens_ = 0;
    session_prompt_tokens_ = 0;
    session_completion_tokens_ = 0;
    session_total_tokens_ = 0;
    session_cache_read_tokens_ = 0;
    session_cache_write_tokens_ = 0;
}

std::string TokenTracker::format_tokens(int count) {
    if (count < 1000) {
        return std::to_string(count);
    }
    std::ostringstream oss;
    if (count < 100000) {
        oss << std::fixed << std::setprecision(1) << (count / 1000.0) << "k";
    } else {
        oss << (count / 1000) << "k";
    }
    return oss.str();
}

std::string TokenTracker::format_status(int context_window) const {
    std::lock_guard<std::mutex> lk(mu_);
    // Display current context occupancy (last prompt tokens), not cumulative total
    return format_tokens(last_prompt_tokens_) + "/" + format_tokens(context_window);
}

} // namespace acecode
