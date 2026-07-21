#include <gtest/gtest.h>

#include "session/compact_checkpoint.hpp"

#include <cstdint>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace {

acecode::ChatMessage msg(std::string role, std::string content) {
    acecode::ChatMessage m;
    m.role = std::move(role);
    m.content = std::move(content);
    return m;
}

acecode::ChatMessage transcript_only(std::string content) {
    auto m = msg("system", std::move(content));
    m.metadata = nlohmann::json{{"transcript_only", true}};
    return m;
}

acecode::ChatMessage malformed_checkpoint() {
    acecode::ChatMessage m;
    m.role = "system";
    m.content = "[bad checkpoint]";
    m.is_meta = true;
    m.subtype = acecode::kCompactCheckpointSubtype;
    m.metadata = nlohmann::json{{"version", acecode::kCompactCheckpointVersion}};
    return m;
}

} // namespace

TEST(CompactCheckpoint, RoundTripsReplacementHistory) {
    acecode::ChatMessage assistant = msg("assistant", "kept assistant");
    assistant.tool_calls = nlohmann::json::array({
        {
            {"id", "call-1"},
            {"type", "function"},
            {"function", {{"name", "bash"}, {"arguments", "{}"}}}
        }
    });

    acecode::CompactCheckpoint checkpoint;
    checkpoint.id = "checkpoint-id";
    checkpoint.timestamp = "2026-06-24T01:02:03Z";
    checkpoint.trigger = "manual";
    checkpoint.summary = "summary";
    checkpoint.messages_compressed = 12;
    checkpoint.estimated_tokens_saved = 345;
    checkpoint.pre_tokens = 1000;
    checkpoint.post_tokens = 200;
    checkpoint.window_number = 3;
    checkpoint.first_window_id = "window-first";
    checkpoint.previous_window_id = "window-previous";
    checkpoint.window_id = "window-current";
    checkpoint.replacement_history = {msg("system", "summary"), assistant};

    auto encoded = acecode::encode_compact_checkpoint(checkpoint);
    EXPECT_TRUE(acecode::is_compact_checkpoint_message(encoded));
    EXPECT_TRUE(encoded.is_meta);
    EXPECT_EQ(encoded.subtype, acecode::kCompactCheckpointSubtype);

    auto decoded = acecode::decode_compact_checkpoint(encoded);
    ASSERT_TRUE(decoded.has_value());
    EXPECT_EQ(decoded->id, "checkpoint-id");
    EXPECT_EQ(decoded->timestamp, "2026-06-24T01:02:03Z");
    EXPECT_EQ(decoded->trigger, "manual");
    EXPECT_EQ(decoded->summary, "summary");
    EXPECT_EQ(decoded->messages_compressed, 12);
    EXPECT_EQ(decoded->estimated_tokens_saved, 345);
    EXPECT_EQ(decoded->window_number, 3);
    EXPECT_EQ(decoded->first_window_id, "window-first");
    EXPECT_EQ(decoded->previous_window_id, "window-previous");
    EXPECT_EQ(decoded->window_id, "window-current");
    ASSERT_EQ(decoded->replacement_history.size(), 2u);
    EXPECT_EQ(decoded->replacement_history[0].content, "summary");
    EXPECT_TRUE(decoded->replacement_history[1].tool_calls.is_array());
}

TEST(CompactCheckpoint, DecodesLegacyVersionWithoutWindowMetadata) {
    acecode::CompactCheckpoint checkpoint;
    checkpoint.id = "legacy-checkpoint-id";
    checkpoint.replacement_history = {msg("user", "legacy summary")};
    auto encoded = acecode::encode_compact_checkpoint(checkpoint);
    encoded.metadata["version"] = 1;
    encoded.metadata.erase("window_number");
    encoded.metadata.erase("first_window_id");
    encoded.metadata.erase("previous_window_id");
    encoded.metadata.erase("window_id");

    auto decoded = acecode::decode_compact_checkpoint(encoded);

    ASSERT_TRUE(decoded.has_value());
    EXPECT_EQ(decoded->version, 1);
    EXPECT_EQ(decoded->id, "legacy-checkpoint-id");
    EXPECT_EQ(decoded->window_number, 0);
    EXPECT_TRUE(decoded->first_window_id.empty());
    EXPECT_TRUE(decoded->previous_window_id.empty());
    EXPECT_TRUE(decoded->window_id.empty());
    ASSERT_EQ(decoded->replacement_history.size(), 1u);
    EXPECT_EQ(decoded->replacement_history[0].content, "legacy summary");
}

TEST(CompactCheckpoint, RoundTripsUnsignedWindowNumberWithoutOverflow) {
    acecode::CompactCheckpoint checkpoint;
    checkpoint.window_number = std::numeric_limits<std::uint64_t>::max();
    checkpoint.replacement_history = {msg("user", "summary")};

    auto decoded = acecode::decode_compact_checkpoint(
        acecode::encode_compact_checkpoint(checkpoint));

    ASSERT_TRUE(decoded.has_value());
    EXPECT_EQ(decoded->window_number,
              std::numeric_limits<std::uint64_t>::max());
}

TEST(CompactCheckpoint, ProviderRelevantMessagesFiltersHiddenRows) {
    auto meta = msg("system", "meta");
    meta.is_meta = true;
    auto pseudo = msg("tool_result", "display only");

    auto filtered = acecode::provider_relevant_messages({
        msg("user", "visible user"),
        meta,
        transcript_only("visible only"),
        pseudo,
        msg("assistant", "visible assistant"),
    });

    ASSERT_EQ(filtered.size(), 2u);
    EXPECT_EQ(filtered[0].content, "visible user");
    EXPECT_EQ(filtered[1].content, "visible assistant");
}

TEST(CompactCheckpoint, ReconstructsFromLatestCheckpointAndSuffix) {
    acecode::CompactCheckpoint first;
    first.trigger = "manual";
    first.replacement_history = {msg("system", "first summary")};
    acecode::CompactCheckpoint second;
    second.trigger = "auto";
    second.replacement_history = {msg("system", "second summary")};

    std::vector<acecode::ChatMessage> raw = {
        msg("user", "old before first"),
        acecode::encode_compact_checkpoint(first),
        msg("user", "after first"),
        acecode::encode_compact_checkpoint(second),
        transcript_only("compact marker"),
        msg("user", "after second"),
    };

    auto effective = acecode::reconstruct_effective_model_history(raw);
    ASSERT_EQ(effective.size(), 2u);
    EXPECT_EQ(effective[0].content, "second summary");
    EXPECT_EQ(effective[1].content, "after second");
}

TEST(CompactCheckpoint, MalformedCheckpointFallsBackToLatestValidCheckpoint) {
    acecode::CompactCheckpoint valid;
    valid.replacement_history = {msg("system", "valid summary")};

    std::vector<acecode::ChatMessage> raw = {
        msg("user", "old before valid"),
        acecode::encode_compact_checkpoint(valid),
        malformed_checkpoint(),
        msg("user", "after malformed"),
    };

    auto effective = acecode::reconstruct_effective_model_history(raw);
    ASSERT_EQ(effective.size(), 2u);
    EXPECT_EQ(effective[0].content, "valid summary");
    EXPECT_EQ(effective[1].content, "after malformed");
}

TEST(CompactCheckpoint, OnlyMalformedCheckpointUsesLegacyVisibleHistory) {
    std::vector<acecode::ChatMessage> raw = {
        msg("user", "legacy old"),
        malformed_checkpoint(),
        msg("assistant", "legacy reply"),
    };

    auto effective = acecode::reconstruct_effective_model_history(raw);
    ASSERT_EQ(effective.size(), 2u);
    EXPECT_EQ(effective[0].content, "legacy old");
    EXPECT_EQ(effective[1].content, "legacy reply");
}
