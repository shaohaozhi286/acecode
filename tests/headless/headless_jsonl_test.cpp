#include <gtest/gtest.h>

#include "headless/headless_jsonl.hpp"

#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using acecode::SessionEvent;
using acecode::SessionEventKind;
using acecode::headless::HeadlessJsonlProjector;
using acecode::headless::JsonlStreamWriter;

namespace {

SessionEvent event(SessionEventKind kind,
                   std::uint64_t seq,
                   std::int64_t timestamp,
                   nlohmann::json payload = nlohmann::json::object()) {
    SessionEvent out;
    out.kind = kind;
    out.seq = seq;
    out.timestamp_ms = timestamp;
    out.payload = std::move(payload);
    return out;
}

void append(std::vector<nlohmann::json>& target,
            std::vector<nlohmann::json> source) {
    for (auto& record : source) target.push_back(std::move(record));
}

std::vector<std::string> types(const std::vector<nlohmann::json>& records) {
    std::vector<std::string> out;
    for (const auto& record : records) {
        out.push_back(record.value("type", std::string{}));
    }
    return out;
}

nlohmann::json finish_payload(int step, const std::string& reason = "stop") {
    return {
        {"step_index", step},
        {"reason", reason},
        {"usage", {
            {"prompt_tokens", 11},
            {"completion_tokens", 7},
            {"total_tokens", 18},
            {"reasoning_tokens", 3},
            {"cache_read_tokens", 2},
            {"cache_write_tokens", 1},
            {"has_data", true},
        }},
    };
}

} // namespace

TEST(HeadlessJsonlProjector, TextTurnUsesCompletedPartSchemaNotTokenDeltas) {
    HeadlessJsonlProjector projector("session-1");
    std::vector<nlohmann::json> records;
    append(records, projector.consume(event(
        SessionEventKind::ModelStepStart, 1, 1000, {{"step_index", 1}})));
    append(records, projector.consume(event(
        SessionEventKind::Token, 2, 1001, {{"text", "hel"}})));
    append(records, projector.consume(event(
        SessionEventKind::Token, 3, 1002, {{"text", "lo"}})));
    append(records, projector.consume(event(
        SessionEventKind::Message, 4, 1003,
        {{"role", "assistant"}, {"content", "hello"}, {"id", "assistant-1"}})));
    append(records, projector.consume(event(
        SessionEventKind::ModelStepFinish, 5, 1004, finish_payload(1))));

    EXPECT_EQ(types(records),
              (std::vector<std::string>{"step_start", "text", "step_finish"}));
    ASSERT_EQ(records.size(), 3u);
    for (const auto& record : records) {
        EXPECT_EQ(record["sessionID"], "session-1");
        EXPECT_TRUE(record["timestamp"].is_number_integer());
        ASSERT_TRUE(record.contains("part"));
        EXPECT_EQ(record["part"]["sessionID"], "session-1");
        EXPECT_FALSE(record["part"].value("id", std::string{}).empty());
        EXPECT_FALSE(record["part"].value("messageID", std::string{}).empty());
    }
    EXPECT_EQ(records[1]["part"]["text"], "hello");
    EXPECT_EQ(records[2]["part"]["reason"], "stop");
    EXPECT_EQ(records[2]["part"]["cost"], 0);
    EXPECT_EQ(records[2]["part"]["tokens"]["input"], 11);
    EXPECT_EQ(records[2]["part"]["tokens"]["cache"]["read"], 2);
}

TEST(HeadlessJsonlProjector, ReasoningIsOptInAndPrecedesCompletedText) {
    for (bool include_reasoning : {false, true}) {
        HeadlessJsonlProjector projector("session-thinking", include_reasoning);
        std::vector<nlohmann::json> records;
        append(records, projector.consume(event(
            SessionEventKind::ModelStepStart, 1, 2000, {{"step_index", 1}})));
        append(records, projector.consume(event(
            SessionEventKind::Reasoning, 2, 2001, {{"text", "think "}})));
        append(records, projector.consume(event(
            SessionEventKind::Reasoning, 3, 2002, {{"text", "carefully"}})));
        append(records, projector.consume(event(
            SessionEventKind::Message, 4, 2003,
            {{"role", "assistant"}, {"content", "answer"}})));
        append(records, projector.consume(event(
            SessionEventKind::ModelStepFinish, 5, 2004, finish_payload(1))));

        if (include_reasoning) {
            EXPECT_EQ(types(records), (std::vector<std::string>{
                "step_start", "reasoning", "text", "step_finish"}));
            EXPECT_EQ(records[1]["part"]["text"], "think carefully");
        } else {
            EXPECT_EQ(types(records), (std::vector<std::string>{
                "step_start", "text", "step_finish"}));
        }
    }
}

TEST(HeadlessJsonlProjector, TranscriptReplaceDropsSupersededReasoning) {
    HeadlessJsonlProjector projector("session-retry", true);
    std::vector<nlohmann::json> records;
    append(records, projector.consume(event(
        SessionEventKind::ModelStepStart, 1, 3000, {{"step_index", 1}})));
    append(records, projector.consume(event(
        SessionEventKind::Reasoning, 2, 3001, {{"text", "old"}})));
    append(records, projector.consume(event(
        SessionEventKind::TranscriptReplace, 3, 3002, {{"messages", nlohmann::json::array()}})));
    append(records, projector.consume(event(
        SessionEventKind::Reasoning, 4, 3003, {{"text", "new"}})));
    append(records, projector.consume(event(
        SessionEventKind::Message, 5, 3004,
        {{"role", "assistant"}, {"content", "done"}})));
    append(records, projector.consume(event(
        SessionEventKind::ModelStepFinish, 6, 3005, finish_payload(1))));

    EXPECT_EQ(types(records), (std::vector<std::string>{
        "step_start", "reasoning", "text", "step_finish"}));
    EXPECT_EQ(records[1]["part"]["text"], "new");
}

TEST(HeadlessJsonlProjector, ToolContinuationProducesCompletedToolBetweenSteps) {
    HeadlessJsonlProjector projector("session-tool", true);
    std::vector<nlohmann::json> records;
    append(records, projector.consume(event(
        SessionEventKind::ModelStepStart, 1, 4000, {{"step_index", 1}})));
    append(records, projector.consume(event(
        SessionEventKind::Reasoning, 2, 4001, {{"text", "inspect"}})));
    append(records, projector.consume(event(
        SessionEventKind::Message, 3, 4002,
        {{"role", "assistant"}, {"content", "before"}})));
    append(records, projector.consume(event(
        SessionEventKind::ToolStart, 4, 4003,
        {{"tool", "bash"}, {"tool_call_id", "call-1"}, {"tool_index", 0},
         {"args", {{"command", "printf tool"}}},
         {"command_preview", "printf tool"}})));
    append(records, projector.consume(event(
        SessionEventKind::ToolEnd, 5, 4004,
        {{"tool", "bash"}, {"tool_call_id", "call-1"}, {"tool_index", 0},
         {"success", true}, {"output", "tool"},
         {"metadata", {{"exit_code", 0}}},
         {"summary", {{"verb", "Ran"}}},
         {"attachments", {{{"type", "file"}, {"url", "file:///result.txt"},
                            {"mime", "text/plain"}}}}})));
    append(records, projector.consume(event(
        SessionEventKind::ModelStepFinish, 6, 4005, finish_payload(1, "tool_calls"))));
    append(records, projector.consume(event(
        SessionEventKind::ModelStepStart, 7, 4006, {{"step_index", 2}})));
    append(records, projector.consume(event(
        SessionEventKind::Message, 8, 4007,
        {{"role", "assistant"}, {"content", "after"}})));
    append(records, projector.consume(event(
        SessionEventKind::ModelStepFinish, 9, 4008, finish_payload(2))));

    EXPECT_EQ(types(records), (std::vector<std::string>{
        "step_start", "reasoning", "text", "tool_use", "step_finish",
        "step_start", "text", "step_finish"}));
    const auto& tool = records[3]["part"];
    EXPECT_EQ(tool["type"], "tool");
    EXPECT_EQ(tool["callID"], "call-1");
    EXPECT_EQ(tool["tool"], "bash");
    EXPECT_EQ(tool["state"]["status"], "completed");
    EXPECT_EQ(tool["state"]["input"]["command"], "printf tool");
    EXPECT_EQ(tool["state"]["output"], "tool");
    EXPECT_EQ(tool["state"]["metadata"]["exit_code"], 0);
    EXPECT_EQ(tool["state"]["metadata"]["summary"]["verb"], "Ran");
    ASSERT_EQ(tool["state"]["attachments"].size(), 1u);
    EXPECT_EQ(tool["state"]["attachments"][0]["type"], "file");
    EXPECT_EQ(tool["state"]["time"]["start"], 4003);
    EXPECT_EQ(tool["state"]["time"]["end"], 4004);
}

TEST(HeadlessJsonlProjector, ProviderErrorFollowsFailedStepFinish) {
    HeadlessJsonlProjector projector("session-error");
    std::vector<nlohmann::json> records;
    append(records, projector.consume(event(
        SessionEventKind::ModelStepStart, 1, 5000, {{"step_index", 1}})));
    append(records, projector.consume(event(
        SessionEventKind::Message, 2, 5001,
        {{"role", "error"}, {"content", "provider exploded"},
         {"metadata", {{"provider_error", {{"kind", "network"}}}}}})));
    EXPECT_EQ(types(records), (std::vector<std::string>{"step_start"}));
    append(records, projector.consume(event(
        SessionEventKind::ModelStepFinish, 3, 5002, finish_payload(1, "error"))));

    EXPECT_EQ(types(records), (std::vector<std::string>{
        "step_start", "step_finish", "error"}));
    EXPECT_EQ(records[2]["error"]["name"], "ProviderError");
    EXPECT_EQ(records[2]["error"]["data"]["message"], "provider exploded");
    EXPECT_EQ(projector.last_error_message(), "provider exploded");
}

TEST(JsonlStreamWriter, WritesOneParseableLineAndFlushesEveryRecord) {
    std::string output;
    int flushes = 0;
    JsonlStreamWriter writer(
        [&](const char* data, std::size_t size) {
            output.append(data, size);
            return true;
        },
        [&] {
            ++flushes;
            return true;
        });

    EXPECT_TRUE(writer.write_record({{"type", "step_start"}, {"sessionID", "s"}}));
    EXPECT_TRUE(writer.write_record({{"type", "step_finish"}, {"sessionID", "s"}}));
    EXPECT_EQ(flushes, 2);
    EXPECT_FALSE(writer.failed());

    std::size_t start = 0;
    int lines = 0;
    while (start < output.size()) {
        const auto end = output.find('\n', start);
        ASSERT_NE(end, std::string::npos);
        const auto parsed = nlohmann::json::parse(output.substr(start, end - start));
        EXPECT_TRUE(parsed.contains("type"));
        ++lines;
        start = end + 1;
    }
    EXPECT_EQ(lines, 2);
}

TEST(JsonlStreamWriter, LatchesWriteAndFlushFailures) {
    JsonlStreamWriter write_failure(
        [](const char*, std::size_t) { return false; }, [] { return true; });
    EXPECT_FALSE(write_failure.write_record({{"type", "error"}}));
    EXPECT_TRUE(write_failure.failed());
    EXPECT_NE(write_failure.error_message().find("write"), std::string::npos);
    EXPECT_FALSE(write_failure.write_record({{"type", "ignored"}}));

    JsonlStreamWriter flush_failure(
        [](const char*, std::size_t) { return true; }, [] { return false; });
    EXPECT_FALSE(flush_failure.write_record({{"type", "error"}}));
    EXPECT_TRUE(flush_failure.failed());
    EXPECT_NE(flush_failure.error_message().find("flush"), std::string::npos);
}

TEST(JsonlStreamWriter, ConcurrentCallsRemainWholeLines) {
    std::string output;
    std::atomic<int> write_failures{0};
    JsonlStreamWriter writer(
        [&](const char* data, std::size_t size) {
            output.append(data, size);
            std::this_thread::yield();
            return true;
        },
        [] { return true; });

    std::vector<std::thread> threads;
    for (int thread = 0; thread < 4; ++thread) {
        threads.emplace_back([&, thread] {
            for (int i = 0; i < 50; ++i) {
                if (!writer.write_record(
                        {{"type", "text"}, {"thread", thread}, {"index", i}})) {
                    ++write_failures;
                }
            }
        });
    }
    for (auto& thread : threads) thread.join();

    int lines = 0;
    std::size_t start = 0;
    while (start < output.size()) {
        const auto end = output.find('\n', start);
        ASSERT_NE(end, std::string::npos);
        EXPECT_NO_THROW(
            (void)nlohmann::json::parse(output.substr(start, end - start)));
        ++lines;
        start = end + 1;
    }
    EXPECT_EQ(write_failures.load(), 0);
    EXPECT_EQ(lines, 200);
}
