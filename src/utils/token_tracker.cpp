#include "token_tracker.hpp"
#include <sstream>
#include <iomanip>

namespace acecode {

void TokenTracker::record(const TokenUsage& usage) {
    std::lock_guard<std::mutex> lk(mu_);
    // Update "last" counters (current context occupancy)
    last_prompt_tokens_ = usage.prompt_tokens;
    last_completion_tokens_ = usage.completion_tokens;
    last_total_tokens_ = usage.total_tokens;
    last_cache_read_tokens_ = usage.cache_read_tokens;
    last_cache_write_tokens_ = usage.cache_write_tokens;
    last_reasoning_tokens_ = usage.reasoning_tokens;
    last_has_data_ = usage.has_data;

    // Accumulate session totals
    session_prompt_tokens_ += usage.prompt_tokens;
    session_completion_tokens_ += usage.completion_tokens;
    session_total_tokens_ += usage.total_tokens;
    session_cache_read_tokens_ += usage.cache_read_tokens;
    session_cache_write_tokens_ += usage.cache_write_tokens;
    session_reasoning_tokens_ += usage.reasoning_tokens;
    session_has_data_ = session_has_data_ || usage.has_data;
}

void TokenTracker::record_estimate(int char_count) {
    std::lock_guard<std::mutex> lk(mu_);
    int estimated = char_count / 4;
    // For estimates, update last_prompt as well (best guess for current context)
    last_prompt_tokens_ = estimated;
    last_completion_tokens_ = 0;
    last_total_tokens_ = estimated;
    last_cache_read_tokens_ = 0;
    last_cache_write_tokens_ = 0;
    last_reasoning_tokens_ = 0;
    last_has_data_ = false;
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

TokenTracker::Snapshot TokenTracker::snapshot() const {
    std::lock_guard<std::mutex> lk(mu_);
    Snapshot out;
    out.last.prompt_tokens = last_prompt_tokens_;
    out.last.completion_tokens = last_completion_tokens_;
    out.last.total_tokens = last_total_tokens_;
    out.last.cache_read_tokens = last_cache_read_tokens_;
    out.last.cache_write_tokens = last_cache_write_tokens_;
    out.last.reasoning_tokens = last_reasoning_tokens_;
    out.last.has_data = last_has_data_;

    out.session.prompt_tokens = session_prompt_tokens_;
    out.session.completion_tokens = session_completion_tokens_;
    out.session.total_tokens = session_total_tokens_;
    out.session.cache_read_tokens = session_cache_read_tokens_;
    out.session.cache_write_tokens = session_cache_write_tokens_;
    out.session.reasoning_tokens = session_reasoning_tokens_;
    out.session.has_data = session_has_data_;
    return out;
}

void TokenTracker::restore(const Snapshot& snapshot) {
    restore(snapshot.last, snapshot.session);
}

void TokenTracker::restore(const TokenUsage& last, const TokenUsage& session) {
    std::lock_guard<std::mutex> lk(mu_);
    last_prompt_tokens_ = last.prompt_tokens;
    last_completion_tokens_ = last.completion_tokens;
    last_total_tokens_ = last.total_tokens;
    last_cache_read_tokens_ = last.cache_read_tokens;
    last_cache_write_tokens_ = last.cache_write_tokens;
    last_reasoning_tokens_ = last.reasoning_tokens;
    last_has_data_ = last.has_data;

    session_prompt_tokens_ = session.prompt_tokens;
    session_completion_tokens_ = session.completion_tokens;
    session_total_tokens_ = session.total_tokens;
    session_cache_read_tokens_ = session.cache_read_tokens;
    session_cache_write_tokens_ = session.cache_write_tokens;
    session_reasoning_tokens_ = session.reasoning_tokens;
    session_has_data_ = session.has_data;
}

void TokenTracker::reset() {
    std::lock_guard<std::mutex> lk(mu_);
    last_prompt_tokens_ = 0;
    last_completion_tokens_ = 0;
    last_total_tokens_ = 0;
    last_cache_read_tokens_ = 0;
    last_cache_write_tokens_ = 0;
    last_reasoning_tokens_ = 0;
    last_has_data_ = false;
    session_prompt_tokens_ = 0;
    session_completion_tokens_ = 0;
    session_total_tokens_ = 0;
    session_cache_read_tokens_ = 0;
    session_cache_write_tokens_ = 0;
    session_reasoning_tokens_ = 0;
    session_has_data_ = false;
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
