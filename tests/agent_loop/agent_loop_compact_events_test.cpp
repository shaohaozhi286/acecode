#include <gtest/gtest.h>

#include "agent_loop.hpp"
#include "commands/compact_prompt.hpp"
#include "permissions.hpp"
#include "provider/llm_provider.hpp"
#include "session/compact_checkpoint.hpp"
#include "session/session_manager.hpp"
#include "session/session_storage.hpp"
#include "tool/tool_executor.hpp"
#include "stub_provider.hpp"

#include <atomic>
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <random>
#include <string>
#include <system_error>

using namespace std::chrono_literals;

namespace {

class CompactEventProvider : public acecode::LlmProvider {
public:
    int chat_calls = 0;
    int stream_calls = 0;
    bool fail_compact = false;
    bool tool_then_text = false;
    bool compact_prompt_saw_full_tool_result = false;
    bool stream_saw_summary = false;
    int stream_message_count = 0;
    std::vector<std::vector<acecode::ChatMessage>> compact_requests;

    acecode::ChatResponse chat(
        const std::vector<acecode::ChatMessage>& messages,
        const std::vector<acecode::ToolDef>&) override {
        ++chat_calls;
        compact_requests.push_back(messages);
        for (const auto& message : messages) {
            if (message.content.find(std::string(128, 'T')) != std::string::npos) {
                compact_prompt_saw_full_tool_result = true;
            }
        }
        acecode::ChatResponse response;
        if (fail_compact) {
            response.content = "compact failed";
            response.finish_reason = "error";
            return response;
        }
        response.content = "Compacted event summary.";
        response.finish_reason = "stop";
        return response;
    }

    void chat_stream(const std::vector<acecode::ChatMessage>& messages,
                     const std::vector<acecode::ToolDef>&,
                     const acecode::StreamCallback& callback,
                     std::atomic<bool>* = nullptr) override {
        ++stream_calls;
        stream_message_count = static_cast<int>(messages.size());
        for (const auto& message : messages) {
            if (message.content.find(acecode::get_compact_summary_prefix()) !=
                std::string::npos) {
                stream_saw_summary = true;
            }
        }

        if (tool_then_text && stream_calls == 1) {
            acecode::StreamEvent tool;
            tool.type = acecode::StreamEventType::ToolCall;
            tool.tool_call.id = "huge-output-call";
            tool.tool_call.function_name = "huge_output";
            tool.tool_call.function_arguments = "{}";
            callback(tool);
        } else {
            acecode::StreamEvent delta;
            delta.type = acecode::StreamEventType::Delta;
            delta.content = "ok";
            callback(delta);
        }
        acecode::StreamEvent done;
        done.type = acecode::StreamEventType::Done;
        callback(done);
    }

    std::string name() const override { return "stub"; }
    bool is_authenticated() override { return true; }
    std::string model() const override { return "stub"; }
    void set_model(const std::string&) override {}
};

acecode::ChatMessage loop_msg(std::string role,
                              std::string content,
                              std::string uuid = {}) {
    acecode::ChatMessage message;
    message.role = std::move(role);
    message.content = std::move(content);
    message.uuid = std::move(uuid);
    return message;
}

void add_history(acecode::AgentLoop& loop, int turns = 5) {
    for (int i = 0; i < turns; ++i) {
        loop.push_message(loop_msg(
            "user", "old user " + std::to_string(i) + " " +
                        std::string(900, 'u')));
        loop.push_message(loop_msg(
            "assistant", "old assistant " + std::to_string(i) + " " +
                             std::string(900, 'a')));
    }
}

acecode::ProviderErrorInfo make_context_overflow_error() {
    acecode::ProviderErrorInfo error;
    error.kind = acecode::ProviderErrorKind::Http;
    error.status_code = 400;
    error.display_message =
        "context_length_exceeded: maximum context length exceeded";
    error.raw_body = R"({"error":{"code":"context_length_exceeded"}})";
    return error;
}

bool request_contains(const std::vector<acecode::ChatMessage>& messages,
                      const std::string& needle) {
    for (const auto& message : messages) {
        if (message.content.find(needle) != std::string::npos) return true;
    }
    return false;
}

std::filesystem::path make_temp_cwd(const std::string& name) {
    auto path = std::filesystem::temp_directory_path() /
                ("acecode_" + name + "_" +
                 std::to_string(std::random_device{}()));
    std::filesystem::remove_all(path);
    std::filesystem::create_directories(path);
    return path;
}

std::vector<acecode::SessionEvent> wait_for_done(
    acecode::AgentLoop& loop,
    std::function<void()> action) {
    std::mutex mutex;
    std::condition_variable cv;
    bool done = false;
    std::vector<acecode::SessionEvent> events;
    const auto subscription = loop.events().subscribe(
        [&](const acecode::SessionEvent& event) {
            std::lock_guard<std::mutex> lock(mutex);
            events.push_back(event);
            if (event.kind == acecode::SessionEventKind::Done) {
                done = true;
                cv.notify_all();
            }
        });
    action();
    {
        std::unique_lock<std::mutex> lock(mutex);
        EXPECT_TRUE(cv.wait_for(lock, 5s, [&] { return done; }));
    }
    loop.events().unsubscribe(subscription);
    return events;
}

bool has_system_event(const std::vector<acecode::SessionEvent>& events,
                      const std::string& needle) {
    for (const auto& event : events) {
        if (event.kind == acecode::SessionEventKind::Message &&
            event.payload.value("role", "") == "system" &&
            event.payload.value("content", "").find(needle) !=
                std::string::npos) {
            return true;
        }
    }
    return false;
}

acecode::ToolImpl huge_output_tool() {
    acecode::ToolImpl tool;
    tool.definition.name = "huge_output";
    tool.definition.description = "Return a large deterministic test payload.";
    tool.definition.parameters = nlohmann::json{
        {"type", "object"},
        {"properties", nlohmann::json::object()},
    };
    tool.is_read_only = true;
    tool.execute = [](const std::string&, const acecode::ToolContext&) {
        acecode::ToolResult result;
        result.output = std::string(800000, 'T');
        return result;
    };
    return tool;
}

} // namespace

TEST(AgentLoopCompactEvents, QueuedCompactAppendsCodexMarkerWithoutTranscriptReplacement) {
    auto provider = std::make_shared<CompactEventProvider>();
    acecode::ToolExecutor tools;
    acecode::PermissionManager permissions;
    acecode::AgentLoop loop(
        [&]() -> std::shared_ptr<acecode::LlmProvider> { return provider; },
        tools, {}, "/tmp/compact-events", permissions);
    add_history(loop, 2);

    const auto events = wait_for_done(loop, [&] { loop.submit_compact(); });

    EXPECT_EQ(provider->chat_calls, 1);
    EXPECT_FALSE(std::any_of(
        events.begin(), events.end(), [](const acecode::SessionEvent& event) {
            return event.kind == acecode::SessionEventKind::TranscriptReplace;
        }));
    EXPECT_TRUE(has_system_event(events, "--- [Compact Checkpoint] ---"));
    EXPECT_TRUE(has_system_event(events, "[Conversation summary]"));
    EXPECT_TRUE(has_system_event(events, "Long threads and multiple compactions"));
    ASSERT_FALSE(loop.messages().empty());
    EXPECT_EQ(loop.messages().back().role, "user");
    EXPECT_EQ(loop.messages().back().content,
              acecode::get_compact_summary_prefix() +
                  "\nCompacted event summary.");
    EXPECT_TRUE(request_contains(loop.messages(), "old user 0"));
    EXPECT_FALSE(request_contains(loop.messages(), "old assistant 0"));
}

TEST(AgentLoopCompactEvents, ManualCompactPersistsAppendOnlyTranscriptAndWindowMetadata) {
    auto provider = std::make_shared<CompactEventProvider>();
    acecode::ToolExecutor tools;
    acecode::PermissionManager permissions;
    auto cwd = make_temp_cwd("manual_compact_checkpoint");
    const std::string project_dir =
        acecode::SessionStorage::get_project_dir(cwd.string());
    std::filesystem::remove_all(project_dir);

    acecode::SessionManager session;
    session.start_session(
        cwd.string(), "stub", "stub-model",
        acecode::SessionStorage::generate_session_id());
    acecode::AgentLoop loop(
        [&]() -> std::shared_ptr<acecode::LlmProvider> { return provider; },
        tools, {}, cwd.string(), permissions);
    loop.set_session_manager(&session);

    auto append = [&](acecode::ChatMessage message) {
        loop.push_message(message);
        session.on_message(message);
    };
    append(loop_msg("user", std::string(900, 'U'), "u-old"));
    append(loop_msg("assistant", std::string(900, 'A')));

    wait_for_done(loop, [&] { loop.submit_compact(); });
    wait_for_done(loop, [&] { loop.submit_compact(); });

    const auto stored = session.load_active_messages();
    EXPECT_TRUE(request_contains(stored, std::string(900, 'U')));
    EXPECT_TRUE(request_contains(stored, std::string(900, 'A')));
    EXPECT_TRUE(request_contains(stored, "--- [Compact Checkpoint] ---"));

    std::vector<acecode::CompactCheckpoint> checkpoints;
    for (const auto& message : stored) {
        if (auto decoded = acecode::decode_compact_checkpoint(message)) {
            checkpoints.push_back(std::move(*decoded));
        }
    }
    ASSERT_EQ(checkpoints.size(), 2u);
    const auto& first = checkpoints[0];
    const auto& second = checkpoints[1];
    EXPECT_EQ(first.version, acecode::kCompactCheckpointVersion);
    EXPECT_EQ(first.window_number, 1);
    EXPECT_EQ(second.window_number, 2);
    EXPECT_FALSE(first.first_window_id.empty());
    EXPECT_FALSE(first.previous_window_id.empty());
    EXPECT_FALSE(first.window_id.empty());
    ASSERT_EQ(first.first_window_id.size(), 36u);
    ASSERT_EQ(first.previous_window_id.size(), 36u);
    ASSERT_EQ(first.window_id.size(), 36u);
    EXPECT_EQ(first.first_window_id[14], '7');
    EXPECT_EQ(first.previous_window_id[14], '7');
    EXPECT_EQ(first.window_id[14], '7');
    EXPECT_NE(std::string("89ab").find(first.window_id[19]), std::string::npos);
    EXPECT_NE(first.previous_window_id, first.window_id);
    EXPECT_EQ(second.first_window_id, first.first_window_id);
    EXPECT_EQ(second.previous_window_id, first.window_id);
    EXPECT_NE(second.window_id, first.window_id);
    EXPECT_TRUE(request_contains(
        second.replacement_history, "Compacted event summary"));
    EXPECT_TRUE(request_contains(
        second.replacement_history, std::string(900, 'U')));
    EXPECT_FALSE(request_contains(
        second.replacement_history, std::string(900, 'A')));

    std::error_code error;
    std::filesystem::remove_all(project_dir, error);
    std::filesystem::remove_all(cwd, error);
}

TEST(AgentLoopCompactEvents, AutoCompactRunsBeforeInitialModelRequest) {
    auto provider = std::make_shared<CompactEventProvider>();
    acecode::ToolExecutor tools;
    acecode::PermissionManager permissions;
    acecode::AgentLoop loop(
        [&]() -> std::shared_ptr<acecode::LlmProvider> { return provider; },
        tools, {}, "/tmp/auto-compact-events", permissions);
    loop.set_context_window(100);
    add_history(loop);

    const auto events = wait_for_done(
        loop, [&] { loop.submit("trigger auto compact"); });

    EXPECT_EQ(provider->chat_calls, 1);
    EXPECT_EQ(provider->stream_calls, 1);
    EXPECT_TRUE(provider->stream_saw_summary);
    EXPECT_TRUE(has_system_event(events, "--- [Compact Checkpoint] ---"));
}

TEST(AgentLoopCompactEvents, LegacyCheckpointSeedsNextCompactWindow) {
    auto provider = std::make_shared<CompactEventProvider>();
    acecode::ToolExecutor tools;
    acecode::PermissionManager permissions;
    auto cwd = make_temp_cwd("legacy_compact_window");
    const std::string project_dir =
        acecode::SessionStorage::get_project_dir(cwd.string());
    std::filesystem::remove_all(project_dir);

    acecode::SessionManager session;
    session.start_session(
        cwd.string(), "stub", "stub-model",
        acecode::SessionStorage::generate_session_id());
    acecode::CompactCheckpoint legacy;
    legacy.version = 1;
    legacy.id = "legacy-checkpoint-window";
    legacy.replacement_history = {loop_msg("user", "legacy retained user")};
    auto legacy_message = acecode::encode_compact_checkpoint(legacy);
    legacy_message.metadata.erase("window_number");
    legacy_message.metadata.erase("first_window_id");
    legacy_message.metadata.erase("previous_window_id");
    legacy_message.metadata.erase("window_id");
    session.on_message(legacy_message);

    acecode::AgentLoop loop(
        [&]() -> std::shared_ptr<acecode::LlmProvider> { return provider; },
        tools, {}, cwd.string(), permissions);
    loop.set_session_manager(&session);
    loop.push_message(loop_msg("user", "legacy retained user"));

    wait_for_done(loop, [&] { loop.submit_compact(); });

    std::optional<acecode::CompactCheckpoint> latest;
    for (const auto& message : session.load_active_messages()) {
        if (auto checkpoint = acecode::decode_compact_checkpoint(message)) {
            latest = std::move(*checkpoint);
        }
    }
    ASSERT_TRUE(latest.has_value());
    EXPECT_EQ(latest->version, acecode::kCompactCheckpointVersion);
    EXPECT_EQ(latest->window_number, 1);
    EXPECT_EQ(latest->first_window_id, "legacy-checkpoint-window");
    EXPECT_EQ(latest->previous_window_id, "legacy-checkpoint-window");
    EXPECT_NE(latest->window_id, "legacy-checkpoint-window");

    std::error_code error;
    std::filesystem::remove_all(project_dir, error);
    std::filesystem::remove_all(cwd, error);
}

TEST(AgentLoopCompactEvents, ForkStartsFreshWindowChainAndResumeKeepsIt) {
    auto provider = std::make_shared<CompactEventProvider>();
    acecode::ToolExecutor tools;
    acecode::PermissionManager permissions;
    auto cwd = make_temp_cwd("fork_compact_window");
    const std::string project_dir =
        acecode::SessionStorage::get_project_dir(cwd.string());
    std::filesystem::remove_all(project_dir);

    acecode::SessionManager source;
    source.start_session(
        cwd.string(), "stub", "stub-model",
        acecode::SessionStorage::generate_session_id());
    acecode::CompactCheckpoint source_checkpoint;
    source_checkpoint.window_number = 9;
    source_checkpoint.first_window_id = "source-first-window";
    source_checkpoint.previous_window_id = "source-previous-window";
    source_checkpoint.window_id = "source-current-window";
    source_checkpoint.replacement_history = {
        loop_msg("user", "retained fork request", "fork-user"),
    };
    source.on_message(acecode::encode_compact_checkpoint(source_checkpoint));

    const auto source_raw = source.load_active_messages();
    const std::string fork_id = source.fork_session_to_new_id(
        source_raw, "fork", source.current_session_id(), "fork-user");
    ASSERT_FALSE(fork_id.empty());

    acecode::SessionManager forked;
    forked.start_session(cwd.string(), "stub", "stub-model");
    const auto fork_raw = forked.resume_session(fork_id);
    ASSERT_FALSE(fork_raw.empty());
    auto inherited = acecode::decode_compact_checkpoint(fork_raw.front());
    ASSERT_TRUE(inherited.has_value());
    EXPECT_EQ(inherited->window_number, 0u);
    EXPECT_NE(inherited->window_id, "source-current-window");

    acecode::AgentLoop loop(
        [&]() -> std::shared_ptr<acecode::LlmProvider> { return provider; },
        tools, {}, cwd.string(), permissions);
    loop.set_session_manager(&forked);
    for (const auto& message :
         acecode::reconstruct_effective_model_history(fork_raw)) {
        loop.push_message(message);
    }

    wait_for_done(loop, [&] { loop.submit_compact(); });

    std::vector<acecode::CompactCheckpoint> checkpoints;
    for (const auto& message : forked.load_active_messages()) {
        if (auto checkpoint = acecode::decode_compact_checkpoint(message)) {
            checkpoints.push_back(std::move(*checkpoint));
        }
    }
    ASSERT_EQ(checkpoints.size(), 2u);
    EXPECT_EQ(checkpoints.back().window_number, 1u);
    EXPECT_EQ(checkpoints.back().first_window_id, inherited->window_id);
    EXPECT_EQ(checkpoints.back().previous_window_id, inherited->window_id);
    EXPECT_NE(checkpoints.back().window_id, inherited->window_id);
    EXPECT_NE(checkpoints.back().window_id, "source-current-window");

    loop.shutdown();
    forked.finalize();
    source.finalize();
    std::error_code error;
    std::filesystem::remove_all(project_dir, error);
    std::filesystem::remove_all(cwd, error);
}

TEST(AgentLoopCompactEvents, ToolFollowUpUsesFullCompactWithoutMicroCompaction) {
    auto provider = std::make_shared<CompactEventProvider>();
    provider->tool_then_text = true;
    acecode::ToolExecutor tools;
    tools.register_tool(huge_output_tool());
    acecode::PermissionManager permissions;
    acecode::AgentLoop loop(
        [&]() -> std::shared_ptr<acecode::LlmProvider> { return provider; },
        tools, {}, "/tmp/mid-turn-compact", permissions);
    loop.set_context_window(200000);

    const auto events = wait_for_done(
        loop, [&] { loop.submit("produce a large tool result"); });

    ASSERT_EQ(provider->stream_calls, 2);
    EXPECT_EQ(provider->chat_calls, 1);
    EXPECT_TRUE(provider->compact_prompt_saw_full_tool_result)
        << "the summarizer must see full tool output; no micro-compaction may clear it first";
    EXPECT_TRUE(provider->stream_saw_summary);
    EXPECT_FALSE(has_system_event(events, "[Micro-compact]"));
}

TEST(AgentLoopCompactEvents, ManySmallMessagesDoNotTriggerStructuralCompaction) {
    auto provider = std::make_shared<CompactEventProvider>();
    acecode::ToolExecutor tools;
    acecode::PermissionManager permissions;
    acecode::AgentLoop loop(
        [&]() -> std::shared_ptr<acecode::LlmProvider> { return provider; },
        tools, {}, "/tmp/no-structural-compact", permissions);
    loop.set_context_window(1000000);
    for (int i = 0; i < 300; ++i) {
        loop.push_message(loop_msg("user", "u" + std::to_string(i)));
        loop.push_message(loop_msg("assistant", "a" + std::to_string(i)));
    }

    wait_for_done(loop, [&] { loop.submit("continue"); });

    EXPECT_EQ(provider->chat_calls, 0);
    EXPECT_EQ(provider->stream_calls, 1);
}

TEST(AgentLoopCompactEvents, FailedAutoCompactIsAtomicAndRetriesOnNextTurn) {
    auto provider = std::make_shared<CompactEventProvider>();
    provider->fail_compact = true;
    acecode::ToolExecutor tools;
    acecode::PermissionManager permissions;
    acecode::AgentLoop loop(
        [&]() -> std::shared_ptr<acecode::LlmProvider> { return provider; },
        tools, {}, "/tmp/auto-compact-failure", permissions);
    loop.set_context_window(100);
    add_history(loop, 2);
    const auto original_provider_size =
        acecode::provider_relevant_messages(loop.messages()).size();

    for (int i = 0; i < 4; ++i) {
        const auto events = wait_for_done(loop, [&] {
            loop.submit("failing compact " + std::to_string(i));
        });
        EXPECT_FALSE(has_system_event(events, "--- [Compact Checkpoint] ---"));
    }

    EXPECT_EQ(provider->chat_calls, 4)
        << "there must be no repeated-failure circuit breaker";
    EXPECT_EQ(provider->stream_calls, 0);
    EXPECT_EQ(acecode::provider_relevant_messages(loop.messages()).size(),
              original_provider_size + 4);
    EXPECT_TRUE(request_contains(loop.messages(), "old assistant 0"));
}

TEST(AgentLoopCompactEvents, NormalRequestContextOverflowDoesNotUseLossyRescue) {
    auto provider = std::make_shared<acecode_test::StubLlmProvider>();
    provider->push_error(make_context_overflow_error());
    acecode::ToolExecutor tools;
    acecode::PermissionManager permissions;
    acecode::AgentLoop loop(
        [&]() -> std::shared_ptr<acecode::LlmProvider> { return provider; },
        tools, {}, "/tmp/no-rescue-compact", permissions);
    loop.set_context_window(1000000);
    add_history(loop, 2);
    const auto original_provider_size =
        acecode::provider_relevant_messages(loop.messages()).size();

    const auto events = wait_for_done(
        loop, [&] { loop.submit("latest user request"); });

    EXPECT_EQ(provider->turn_count(), 1);
    EXPECT_EQ(acecode::provider_relevant_messages(loop.messages()).size(),
              original_provider_size + 1);
    EXPECT_TRUE(request_contains(loop.messages(), "old assistant 0"));
    EXPECT_FALSE(has_system_event(events, "[Rescue compact]"));
    EXPECT_FALSE(has_system_event(events, "--- [Compact Checkpoint] ---"));
}
