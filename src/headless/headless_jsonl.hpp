#pragma once

// OpenCode-style completed-part JSONL support for `acecode -p`.
// The projector is pure stateful translation: it never writes files/stdout.
// JsonlStreamWriter owns the checked, line-atomic, flush-per-record sink.

#include "../session/session_client.hpp"

#include <nlohmann/json.hpp>

#include <cstdio>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace acecode::headless {

class HeadlessJsonlProjector {
public:
    explicit HeadlessJsonlProjector(std::string session_id,
                                    bool include_reasoning = false);

    // One ordered SessionEvent can produce zero or more completed-part records.
    std::vector<nlohmann::json> consume(const SessionEvent& event);

    // Used for runner-owned terminal states such as Ctrl+C after the session
    // event stream has closed.
    nlohmann::json make_error_record(const std::string& name,
                                     const std::string& message,
                                     nlohmann::json details,
                                     std::int64_t timestamp_ms) const;

    const std::string& last_error_message() const { return last_error_message_; }

private:
    struct ToolStartState {
        std::string tool;
        nlohmann::json input = nlohmann::json::object();
        std::string title;
        std::int64_t started_at_ms = 0;
    };

    nlohmann::json wrap_part(const std::string& type,
                             std::int64_t timestamp_ms,
                             nlohmann::json part) const;
    nlohmann::json base_part(const std::string& id,
                             const std::string& type) const;
    void flush_reasoning(std::vector<nlohmann::json>& output,
                         std::int64_t completed_at_ms);
    void queue_or_emit_error(std::vector<nlohmann::json>& output,
                             nlohmann::json record);
    std::string tool_key(const nlohmann::json& payload) const;
    std::string active_message_id() const;

    std::string session_id_;
    bool include_reasoning_ = false;
    bool step_active_ = false;
    std::uint64_t step_start_seq_ = 0;
    std::int64_t step_started_at_ms_ = 0;
    std::string message_id_;
    std::string reasoning_text_;
    std::uint64_t reasoning_first_seq_ = 0;
    std::int64_t reasoning_started_at_ms_ = 0;
    std::unordered_map<std::string, ToolStartState> tool_starts_;
    std::vector<nlohmann::json> pending_errors_;
    std::unordered_set<std::string> emitted_error_signatures_;
    std::string last_error_message_;
};

class JsonlStreamWriter {
public:
    using WriteFn = std::function<bool(const char*, std::size_t)>;
    using FlushFn = std::function<bool()>;

    explicit JsonlStreamWriter(std::FILE* file);
    JsonlStreamWriter(WriteFn write_fn, FlushFn flush_fn);

    bool write_record(const nlohmann::json& record);
    bool failed() const;
    std::string error_message() const;

private:
    mutable std::mutex mu_;
    WriteFn write_fn_;
    FlushFn flush_fn_;
    bool failed_ = false;
    std::string error_message_;
};

} // namespace acecode::headless
