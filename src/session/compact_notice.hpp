#pragma once

#include "../provider/llm_provider.hpp"

#include <nlohmann/json.hpp>

#include <optional>
#include <string>

namespace acecode {

inline constexpr const char* kCompactNoticeFlagKey = "compact_notice";
inline constexpr const char* kCompactNoticeIdKey = "compact_notice_id";
inline constexpr const char* kCompactNoticeStageKey = "compact_notice_stage";
inline constexpr const char* kCompactNoticeCompleteKey = "compact_notice_complete";

struct CompactNotice {
    std::string id;
    std::string stage;
    bool complete = false;
};

nlohmann::json make_compact_notice_metadata(const std::string& id,
                                            const std::string& stage,
                                            bool complete = false);

std::optional<CompactNotice> decode_compact_notice(
    const nlohmann::json& metadata);

std::optional<CompactNotice> decode_compact_notice(const ChatMessage& message);

} // namespace acecode
