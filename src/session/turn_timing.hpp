#pragma once

#include "../provider/llm_provider.hpp"

#include <cstdint>
#include <algorithm>
#include <optional>
#include <string>

#include <nlohmann/json.hpp>

namespace acecode {

struct TurnTimingRecord {
    std::string user_message_uuid;
    std::int64_t started_at_ms = 0;
    std::int64_t completed_at_ms = 0;
    std::int64_t duration_ms = 0;
    std::string status;
};

inline bool is_valid_turn_timing_status(const std::string& status) {
    return status == "completed" || status == "aborted" || status == "error";
}

inline std::int64_t non_negative_turn_timing_i64(const nlohmann::json& value) {
    if (!value.is_number_integer() && !value.is_number_unsigned() && !value.is_number_float()) {
        return 0;
    }
    const double raw = value.get<double>();
    if (raw <= 0.0) return 0;
    return static_cast<std::int64_t>(raw);
}

inline nlohmann::json encode_turn_timing(const TurnTimingRecord& timing) {
    const std::int64_t started = std::max<std::int64_t>(0, timing.started_at_ms);
    const std::int64_t completed = std::max<std::int64_t>(0, timing.completed_at_ms);
    const std::int64_t inferred_duration = completed >= started ? completed - started : 0;
    const std::int64_t duration = timing.duration_ms >= 0 ? timing.duration_ms : inferred_duration;
    return nlohmann::json{
        {"user_message_uuid", timing.user_message_uuid},
        {"started_at_ms", started},
        {"completed_at_ms", completed},
        {"duration_ms", std::max<std::int64_t>(0, duration)},
        {"status", is_valid_turn_timing_status(timing.status) ? timing.status : "completed"},
    };
}

inline std::optional<TurnTimingRecord> decode_turn_timing(const nlohmann::json& value) {
    if (!value.is_object()) return std::nullopt;
    TurnTimingRecord timing;
    timing.user_message_uuid = value.value("user_message_uuid", std::string{});
    timing.started_at_ms = non_negative_turn_timing_i64(value.value("started_at_ms", nlohmann::json{}));
    timing.completed_at_ms = non_negative_turn_timing_i64(value.value("completed_at_ms", nlohmann::json{}));
    timing.duration_ms = non_negative_turn_timing_i64(value.value("duration_ms", nlohmann::json{}));
    timing.status = value.value("status", std::string{});
    if (timing.user_message_uuid.empty()) return std::nullopt;
    if (!is_valid_turn_timing_status(timing.status)) return std::nullopt;
    if (timing.duration_ms == 0 && timing.completed_at_ms >= timing.started_at_ms) {
        timing.duration_ms = timing.completed_at_ms - timing.started_at_ms;
    }
    return timing;
}

inline ChatMessage make_turn_timing_message(const TurnTimingRecord& timing,
                                            const std::string& timestamp) {
    ChatMessage msg;
    msg.role = "system";
    msg.content = "[Turn timing]";
    msg.timestamp = timestamp;
    msg.metadata = nlohmann::json::object();
    msg.metadata["transcript_only"] = true;
    msg.metadata["turn_timing"] = encode_turn_timing(timing);
    return msg;
}

inline bool is_turn_timing_message(const ChatMessage& msg) {
    if (!msg.metadata.is_object()) return false;
    return decode_turn_timing(msg.metadata.value("turn_timing", nlohmann::json{})).has_value();
}

inline std::string turn_timing_user_message_uuid(const ChatMessage& msg) {
    if (!msg.metadata.is_object()) return {};
    auto timing = decode_turn_timing(msg.metadata.value("turn_timing", nlohmann::json{}));
    return timing.has_value() ? timing->user_message_uuid : std::string{};
}

} // namespace acecode
