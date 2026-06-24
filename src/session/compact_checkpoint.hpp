#pragma once

#include "../provider/llm_provider.hpp"

#include <optional>
#include <string>
#include <vector>

namespace acecode {

constexpr const char* kCompactCheckpointSubtype = "compact_checkpoint";
constexpr int kCompactCheckpointVersion = 1;

struct CompactCheckpoint {
    int version = kCompactCheckpointVersion;
    std::string id;
    std::string timestamp;
    std::string trigger;
    std::string summary;
    int messages_compressed = 0;
    int estimated_tokens_saved = 0;
    int pre_tokens = 0;
    int post_tokens = 0;
    std::vector<ChatMessage> replacement_history;
};

bool is_compact_checkpoint_message(const ChatMessage& msg);

ChatMessage encode_compact_checkpoint(const CompactCheckpoint& checkpoint);

std::optional<CompactCheckpoint> decode_compact_checkpoint(const ChatMessage& msg);

std::vector<ChatMessage> provider_relevant_messages(const std::vector<ChatMessage>& messages);

std::vector<ChatMessage> reconstruct_effective_model_history(
    const std::vector<ChatMessage>& raw_messages);

} // namespace acecode
