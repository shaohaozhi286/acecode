#include "headless_jsonl.hpp"

#include <chrono>
#include <utility>

namespace acecode::headless {

namespace {

std::int64_t fallback_now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

std::int64_t event_time(const SessionEvent& event) {
    return event.timestamp_ms > 0 ? event.timestamp_ms : fallback_now_ms();
}

nlohmann::json object_or_raw(const nlohmann::json& value) {
    if (value.is_object()) return value;
    return nlohmann::json{{"raw", value}};
}

std::string error_message_from_record(const nlohmann::json& record) {
    if (!record.is_object() || !record.contains("error") ||
        !record["error"].is_object()) {
        return {};
    }
    const auto& error = record["error"];
    if (error.contains("data") && error["data"].is_object()) {
        return error["data"].value("message", std::string{});
    }
    return {};
}

} // namespace

HeadlessJsonlProjector::HeadlessJsonlProjector(std::string session_id,
                                               bool include_reasoning)
    : session_id_(std::move(session_id)),
      include_reasoning_(include_reasoning) {}

nlohmann::json HeadlessJsonlProjector::base_part(const std::string& id,
                                                 const std::string& type) const {
    return nlohmann::json{
        {"id", id},
        {"sessionID", session_id_},
        {"messageID", active_message_id()},
        {"type", type},
    };
}

nlohmann::json HeadlessJsonlProjector::wrap_part(
    const std::string& type,
    std::int64_t timestamp_ms,
    nlohmann::json part) const {
    return nlohmann::json{
        {"type", type},
        {"timestamp", timestamp_ms},
        {"sessionID", session_id_},
        {"part", std::move(part)},
    };
}

std::string HeadlessJsonlProjector::active_message_id() const {
    if (!message_id_.empty()) return message_id_;
    if (step_start_seq_ > 0) return "message-" + std::to_string(step_start_seq_);
    return "message-0";
}

std::string HeadlessJsonlProjector::tool_key(const nlohmann::json& payload) const {
    const std::string call_id = payload.value("tool_call_id", std::string{});
    if (!call_id.empty()) return "call:" + call_id;
    const int tool_index = payload.value("tool_index", -1);
    if (tool_index >= 0) return "index:" + std::to_string(tool_index);
    return "tool:" + payload.value("tool", std::string{});
}

void HeadlessJsonlProjector::flush_reasoning(
    std::vector<nlohmann::json>& output,
    std::int64_t completed_at_ms) {
    if (reasoning_text_.empty()) return;
    if (include_reasoning_) {
        const std::uint64_t id_seq = reasoning_first_seq_ > 0
            ? reasoning_first_seq_
            : step_start_seq_;
        auto part = base_part("part-" + std::to_string(id_seq), "reasoning");
        part["text"] = reasoning_text_;
        part["time"] = {
            {"start", reasoning_started_at_ms_ > 0
                          ? reasoning_started_at_ms_
                          : step_started_at_ms_},
            {"end", completed_at_ms},
        };
        output.push_back(wrap_part("reasoning", completed_at_ms, std::move(part)));
    }
    reasoning_text_.clear();
    reasoning_first_seq_ = 0;
    reasoning_started_at_ms_ = 0;
}

nlohmann::json HeadlessJsonlProjector::make_error_record(
    const std::string& name,
    const std::string& message,
    nlohmann::json details,
    std::int64_t timestamp_ms) const {
    if (!details.is_object()) details = nlohmann::json{{"details", std::move(details)}};
    details["message"] = message;
    return nlohmann::json{
        {"type", "error"},
        {"timestamp", timestamp_ms > 0 ? timestamp_ms : fallback_now_ms()},
        {"sessionID", session_id_},
        {"error", {
            {"name", name.empty() ? "Error" : name},
            {"data", std::move(details)},
        }},
    };
}

void HeadlessJsonlProjector::queue_or_emit_error(
    std::vector<nlohmann::json>& output,
    nlohmann::json record) {
    const std::string signature = record.value("sessionID", std::string{}) + "\n" +
        std::to_string(record.value("timestamp", std::int64_t{0})) + "\n" +
        record.value("error", nlohmann::json::object()).dump();
    if (!emitted_error_signatures_.insert(signature).second) return;
    last_error_message_ = error_message_from_record(record);
    if (step_active_) {
        pending_errors_.push_back(std::move(record));
    } else {
        output.push_back(std::move(record));
    }
}

std::vector<nlohmann::json> HeadlessJsonlProjector::consume(
    const SessionEvent& event) {
    std::vector<nlohmann::json> output;
    const std::int64_t timestamp_ms = event_time(event);

    switch (event.kind) {
    case SessionEventKind::ModelStepStart: {
        // Defensive close is intentionally omitted:AgentLoop owns the invariant
        // that every start has a finish before the next start.
        step_active_ = true;
        step_start_seq_ = event.seq;
        step_started_at_ms_ = timestamp_ms;
        message_id_ = "message-" + std::to_string(event.seq);
        reasoning_text_.clear();
        reasoning_first_seq_ = 0;
        reasoning_started_at_ms_ = 0;
        tool_starts_.clear();
        pending_errors_.clear();

        auto part = base_part("part-" + std::to_string(event.seq), "step-start");
        output.push_back(wrap_part("step_start", timestamp_ms, std::move(part)));
        break;
    }
    case SessionEventKind::Reasoning:
        if (event.payload.contains("text") && event.payload["text"].is_string()) {
            if (reasoning_text_.empty()) {
                reasoning_first_seq_ = event.seq;
                reasoning_started_at_ms_ = timestamp_ms;
            }
            reasoning_text_ += event.payload["text"].get<std::string>();
        }
        break;
    case SessionEventKind::TranscriptReplace:
        reasoning_text_.clear();
        reasoning_first_seq_ = 0;
        reasoning_started_at_ms_ = 0;
        break;
    case SessionEventKind::Message: {
        const std::string role = event.payload.value("role", std::string{});
        if (role == "assistant") {
            flush_reasoning(output, timestamp_ms);
            const std::string text = event.payload.value("content", std::string{});
            if (!text.empty()) {
                auto part = base_part("part-" + std::to_string(event.seq), "text");
                part["text"] = text;
                part["time"] = {
                    {"start", step_started_at_ms_},
                    {"end", timestamp_ms},
                };
                output.push_back(wrap_part("text", timestamp_ms, std::move(part)));
            }
        } else if (role == "error") {
            nlohmann::json details = nlohmann::json::object();
            std::string name = "Error";
            if (event.payload.contains("metadata") &&
                event.payload["metadata"].is_object()) {
                details = event.payload["metadata"];
                if (details.contains("provider_error")) name = "ProviderError";
            }
            queue_or_emit_error(
                output,
                make_error_record(name,
                                  event.payload.value("content", std::string{}),
                                  std::move(details), timestamp_ms));
        }
        break;
    }
    case SessionEventKind::ToolStart: {
        flush_reasoning(output, timestamp_ms);
        ToolStartState start;
        start.tool = event.payload.value("tool", std::string{});
        start.input = object_or_raw(
            event.payload.value("args", nlohmann::json::object()));
        start.title = event.payload.value("command_preview", start.tool);
        start.started_at_ms = timestamp_ms;
        tool_starts_[tool_key(event.payload)] = std::move(start);
        break;
    }
    case SessionEventKind::ToolEnd: {
        flush_reasoning(output, timestamp_ms);
        const std::string key = tool_key(event.payload);
        ToolStartState start;
        auto it = tool_starts_.find(key);
        if (it != tool_starts_.end()) {
            start = std::move(it->second);
            tool_starts_.erase(it);
        } else {
            start.tool = event.payload.value("tool", std::string{});
            start.started_at_ms = timestamp_ms;
            start.title = start.tool;
        }

        const std::string call_id = event.payload.value("tool_call_id", key);
        auto part = base_part("part-" + std::to_string(event.seq), "tool");
        part["callID"] = call_id;
        part["tool"] = event.payload.value("tool", start.tool);

        nlohmann::json metadata = event.payload.value(
            "metadata", nlohmann::json::object());
        if (!metadata.is_object()) metadata = object_or_raw(metadata);
        for (const char* field : {"summary", "hunks", "attachment_warnings"}) {
            if (event.payload.contains(field)) metadata[field] = event.payload[field];
        }
        const bool success = event.payload.value("success", false);
        if (success) {
            nlohmann::json state = {
                {"status", "completed"},
                {"input", start.input},
                {"output", event.payload.value("output", std::string{})},
                {"title", start.title.empty() ? start.tool : start.title},
                {"metadata", std::move(metadata)},
                {"time", {
                    {"start", start.started_at_ms},
                    {"end", timestamp_ms},
                }},
            };
            if (event.payload.contains("attachments") &&
                event.payload["attachments"].is_array() &&
                !event.payload["attachments"].empty()) {
                state["attachments"] = event.payload["attachments"];
            }
            part["state"] = std::move(state);
        } else {
            const std::string error = event.payload.value(
                "output", std::string{"Tool execution failed"});
            part["state"] = {
                {"status", "error"},
                {"input", start.input},
                {"error", error},
                {"metadata", std::move(metadata)},
                {"time", {
                    {"start", start.started_at_ms},
                    {"end", timestamp_ms},
                }},
            };
        }
        output.push_back(wrap_part("tool_use", timestamp_ms, std::move(part)));
        break;
    }
    case SessionEventKind::Error: {
        const std::string message = event.payload.value(
            "reason", event.payload.value("message", std::string{"Session error"}));
        queue_or_emit_error(
            output,
            make_error_record("SessionError", message, event.payload, timestamp_ms));
        break;
    }
    case SessionEventKind::ModelStepFinish: {
        flush_reasoning(output, timestamp_ms);
        auto part = base_part("part-" + std::to_string(event.seq), "step-finish");
        part["reason"] = event.payload.value("reason", std::string{"unknown"});
        part["cost"] = 0;
        const auto usage = event.payload.value("usage", nlohmann::json::object());
        part["tokens"] = {
            {"total", usage.value("total_tokens", 0)},
            {"input", usage.value("prompt_tokens", 0)},
            {"output", usage.value("completion_tokens", 0)},
            {"reasoning", usage.value("reasoning_tokens", 0)},
            {"cache", {
                {"read", usage.value("cache_read_tokens", 0)},
                {"write", usage.value("cache_write_tokens", 0)},
            }},
        };
        output.push_back(wrap_part("step_finish", timestamp_ms, std::move(part)));
        for (auto& error : pending_errors_) output.push_back(std::move(error));
        pending_errors_.clear();
        step_active_ = false;
        step_start_seq_ = 0;
        step_started_at_ms_ = 0;
        message_id_.clear();
        tool_starts_.clear();
        break;
    }
    default:
        break;
    }
    return output;
}

JsonlStreamWriter::JsonlStreamWriter(std::FILE* file)
    : JsonlStreamWriter(
          [file](const char* data, std::size_t size) {
              return file != nullptr && std::fwrite(data, 1, size, file) == size;
          },
          [file]() { return file != nullptr && std::fflush(file) == 0; }) {}

JsonlStreamWriter::JsonlStreamWriter(WriteFn write_fn, FlushFn flush_fn)
    : write_fn_(std::move(write_fn)), flush_fn_(std::move(flush_fn)) {}

bool JsonlStreamWriter::write_record(const nlohmann::json& record) {
    const std::string line = record.dump() + "\n";
    std::lock_guard<std::mutex> lk(mu_);
    if (failed_) return false;
    if (!write_fn_ || !write_fn_(line.data(), line.size())) {
        failed_ = true;
        error_message_ = "failed to write JSONL record to stdout";
        return false;
    }
    if (!flush_fn_ || !flush_fn_()) {
        failed_ = true;
        error_message_ = "failed to flush JSONL record to stdout";
        return false;
    }
    return true;
}

bool JsonlStreamWriter::failed() const {
    std::lock_guard<std::mutex> lk(mu_);
    return failed_;
}

std::string JsonlStreamWriter::error_message() const {
    std::lock_guard<std::mutex> lk(mu_);
    return error_message_;
}

} // namespace acecode::headless
