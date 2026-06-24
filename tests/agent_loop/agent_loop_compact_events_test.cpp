#include <gtest/gtest.h>

#include "agent_loop.hpp"
#include "commands/compact.hpp"
#include "permissions.hpp"
#include "provider/llm_provider.hpp"
#include "session/compact_checkpoint.hpp"
#include "session/session_manager.hpp"
#include "session/session_storage.hpp"
#include "tool/tool_executor.hpp"
#include "stub_provider.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <functional>
#include <mutex>
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
    bool compact_prompt_saw_cleared_tool_result = false;
    bool stream_saw_summary = false;
    int stream_message_count = 0;

    acecode::ChatResponse chat(const std::vector<acecode::ChatMessage>&,
                               const std::vector<acecode::ToolDef>&) override {
        chat_calls++;
        acecode::ChatResponse resp;
        if (fail_compact) {
            resp.content = "compact failed";
            resp.finish_reason = "error";
            return resp;
        }
        resp.content = "<summary>Compacted event summary.</summary>";
        resp.finish_reason = "stop";
        return resp;
    }

    void chat_stream(const std::vector<acecode::ChatMessage>& messages,
                     const std::vector<acecode::ToolDef>&,
                     const acecode::StreamCallback& callback,
                     std::atomic<bool>* = nullptr) override {
        stream_calls++;
        stream_message_count = static_cast<int>(messages.size());
        for (const auto& msg : messages) {
            if (msg.content.find("Compacted event summary") != std::string::npos) {
                stream_saw_summary = true;
            }
        }
        acecode::StreamEvent delta;
        delta.type = acecode::StreamEventType::Delta;
        delta.content = "ok";
        callback(delta);
    }

    std::string name() const override { return "stub"; }
    bool is_authenticated() override { return true; }
    std::string model() const override { return "stub"; }
    void set_model(const std::string&) override {}
};

acecode::ChatMessage loop_msg(std::string role, std::string content, std::string uuid = {}) {
    acecode::ChatMessage msg;
    msg.role = std::move(role);
    msg.content = std::move(content);
    msg.uuid = std::move(uuid);
    return msg;
}

acecode::ChatMessage tool_call_msg(const std::string& call_id, const std::string& tool_name) {
    acecode::ChatMessage msg;
    msg.role = "assistant";
    msg.content = "";
    msg.tool_calls = nlohmann::json::array({
        {
            {"id", call_id},
            {"function", {{"name", tool_name}, {"arguments", "{}"}}}
        }
    });
    return msg;
}

acecode::ChatMessage tool_result_msg(const std::string& call_id, std::string content) {
    acecode::ChatMessage msg;
    msg.role = "tool";
    msg.tool_call_id = call_id;
    msg.content = std::move(content);
    return msg;
}

void add_compactable_history(acecode::AgentLoop& loop, int turns = 5) {
    for (int i = 0; i < turns; ++i) {
        loop.push_message(loop_msg("user", "old user " + std::to_string(i) + " " + std::string(900, 'u')));
        loop.push_message(loop_msg("assistant", "old assistant " + std::to_string(i) + " " + std::string(900, 'a')));
    }
}

acecode::ProviderErrorInfo make_context_overflow_error() {
    acecode::ProviderErrorInfo error;
    error.kind = acecode::ProviderErrorKind::Http;
    error.status_code = 400;
    error.display_message = "context_length_exceeded: maximum context length exceeded";
    error.raw_body = R"({"error":{"code":"context_length_exceeded"}})";
    return error;
}

bool request_contains(const std::vector<acecode::ChatMessage>& messages,
                      const std::string& needle) {
    for (const auto& msg : messages) {
        if (msg.content.find(needle) != std::string::npos) {
            return true;
        }
    }
    return false;
}

std::filesystem::path make_temp_cwd(const std::string& name) {
    auto path = std::filesystem::temp_directory_path() /
                ("acecode_" + name + "_" + std::to_string(std::random_device{}()));
    std::filesystem::remove_all(path);
    std::filesystem::create_directories(path);
    return path;
}

std::vector<acecode::SessionEvent> wait_for_done(
    acecode::AgentLoop& loop,
    std::function<void()> action) {
    std::mutex mu;
    std::condition_variable cv;
    bool done = false;
    std::vector<acecode::SessionEvent> events;
    auto sub = loop.events().subscribe([&](const acecode::SessionEvent& evt) {
        std::lock_guard<std::mutex> lk(mu);
        events.push_back(evt);
        if (evt.kind == acecode::SessionEventKind::Done) {
            done = true;
            cv.notify_all();
        }
    });
    action();
    {
        std::unique_lock<std::mutex> lk(mu);
        EXPECT_TRUE(cv.wait_for(lk, 5s, [&] { return done; }));
    }
    loop.events().unsubscribe(sub);
    return events;
}

} // namespace

TEST(AgentLoopCompactEvents, QueuedCompactAppendsMarkerWithoutTranscriptReplacement) {
    auto provider = std::make_shared<CompactEventProvider>();
    acecode::ToolExecutor tools;
    acecode::PermissionManager permissions;

    std::mutex mu;
    std::condition_variable cv;
    bool done = false;

    acecode::AgentCallbacks cb;

    acecode::AgentLoop loop(
        [&]() -> std::shared_ptr<acecode::LlmProvider> { return provider; },
        tools,
        cb,
        "/tmp/compact-events",
        permissions);

    std::vector<acecode::SessionEvent> events;
    auto sub = loop.events().subscribe([&](const acecode::SessionEvent& evt) {
        std::lock_guard<std::mutex> lk(mu);
        events.push_back(evt);
        if (evt.kind == acecode::SessionEventKind::Done) {
            done = true;
            cv.notify_all();
        }
    });

    loop.push_message(loop_msg("user", std::string(900, 'a'), "u-old"));
    loop.push_message(loop_msg("assistant", std::string(900, 'b')));
    for (int i = 0; i < 4; ++i) {
        loop.push_message(loop_msg("user", "keep " + std::to_string(i), "u-keep-" + std::to_string(i)));
        loop.push_message(loop_msg("assistant", "kept " + std::to_string(i)));
    }
    loop.submit_compact();

    {
        std::unique_lock<std::mutex> lk(mu);
        ASSERT_TRUE(cv.wait_for(lk, 5s, [&] { return done; }));
    }

    std::vector<acecode::SessionEvent> snapshot;
    {
        std::lock_guard<std::mutex> lk(mu);
        snapshot = events;
    }

    bool saw_replace = false;
    bool saw_completion = false;
    for (const auto& evt : snapshot) {
        if (evt.kind == acecode::SessionEventKind::TranscriptReplace) {
            saw_replace = true;
        }
        if (evt.kind == acecode::SessionEventKind::Message &&
            evt.payload.value("role", "") == "system" &&
            evt.payload.value("content", "").find("Compacted 2 messages") != std::string::npos) {
            saw_completion = true;
        }
    }

    EXPECT_FALSE(saw_replace);
    EXPECT_TRUE(saw_completion);
    EXPECT_TRUE(request_contains(loop.messages(), "Compacted event summary"));
    EXPECT_FALSE(request_contains(loop.messages(), std::string(900, 'a')));
    loop.events().unsubscribe(sub);
}

TEST(AgentLoopCompactEvents, ManualCompactAppendsCheckpointWithoutRewritingSessionTranscript) {
    auto provider = std::make_shared<CompactEventProvider>();
    acecode::ToolExecutor tools;
    acecode::PermissionManager permissions;
    acecode::AgentCallbacks cb;
    auto cwd = make_temp_cwd("manual_compact_checkpoint");
    const std::string project_dir = acecode::SessionStorage::get_project_dir(cwd.string());
    std::filesystem::remove_all(project_dir);

    acecode::SessionManager sm;
    const std::string session_id = acecode::SessionStorage::generate_session_id();
    sm.start_session(cwd.string(), "stub", "stub-model", session_id);

    acecode::AgentLoop loop(
        [&]() -> std::shared_ptr<acecode::LlmProvider> { return provider; },
        tools,
        cb,
        cwd.string(),
        permissions);
    loop.set_session_manager(&sm);

    auto add = [&](acecode::ChatMessage message) {
        loop.push_message(message);
        sm.on_message(message);
    };
    add(loop_msg("user", std::string(900, 'a'), "u-old"));
    add(loop_msg("assistant", std::string(900, 'b')));
    for (int i = 0; i < 4; ++i) {
        add(loop_msg("user", "keep " + std::to_string(i), "u-keep-" + std::to_string(i)));
        add(loop_msg("assistant", "kept " + std::to_string(i)));
    }

    wait_for_done(loop, [&] {
        loop.submit_compact();
    });

    auto stored = sm.load_active_messages();
    EXPECT_TRUE(request_contains(stored, std::string(900, 'a')));
    int checkpoint_count = 0;
    std::vector<acecode::ChatMessage> replacement_history;
    for (const auto& message : stored) {
        auto checkpoint = acecode::decode_compact_checkpoint(message);
        if (!checkpoint.has_value()) continue;
        checkpoint_count++;
        replacement_history = checkpoint->replacement_history;
    }

    EXPECT_EQ(checkpoint_count, 1);
    EXPECT_TRUE(request_contains(replacement_history, "Compacted event summary"));
    EXPECT_FALSE(request_contains(replacement_history, std::string(900, 'a')));
    EXPECT_TRUE(request_contains(loop.messages(), "Compacted event summary"));
    EXPECT_FALSE(request_contains(loop.messages(), std::string(900, 'a')));

    std::error_code ec;
    std::filesystem::remove_all(project_dir, ec);
    std::filesystem::remove_all(cwd, ec);
}

TEST(AgentLoopCompactEvents, AutoCompactRunsWithoutCallbackBeforeChatStream) {
    auto provider = std::make_shared<CompactEventProvider>();
    acecode::ToolExecutor tools;
    acecode::PermissionManager permissions;
    acecode::AgentCallbacks cb;

    acecode::AgentLoop loop(
        [&]() -> std::shared_ptr<acecode::LlmProvider> { return provider; },
        tools,
        cb,
        "/tmp/auto-compact-events",
        permissions);
    loop.set_context_window(1);
    add_compactable_history(loop);

    auto events = wait_for_done(loop, [&] {
        loop.submit("trigger auto compact");
    });

    EXPECT_GE(provider->chat_calls, 1);
    EXPECT_EQ(provider->stream_calls, 1);
    EXPECT_TRUE(provider->stream_saw_summary)
        << "chat_stream must receive compacted history, not the oversized original";

    bool saw_replace = false;
    bool saw_completion = false;
    for (const auto& evt : events) {
        if (evt.kind == acecode::SessionEventKind::TranscriptReplace) {
            saw_replace = true;
        }
        if (evt.kind == acecode::SessionEventKind::Message &&
            evt.payload.value("role", "") == "system" &&
            evt.payload.value("content", "").find("[Auto-compact] Compacted") != std::string::npos) {
            saw_completion = true;
        }
    }
    EXPECT_FALSE(saw_replace);
    EXPECT_TRUE(saw_completion);
}

TEST(AgentLoopCompactEvents, AutoCompactRunsMicroCompactBeforeFullCompact) {
    class MicroProvider : public CompactEventProvider {
    public:
        acecode::ChatResponse chat(const std::vector<acecode::ChatMessage>& messages,
                                   const std::vector<acecode::ToolDef>& tools) override {
            for (const auto& msg : messages) {
                if (msg.content.find("[Old tool result content cleared]") != std::string::npos) {
                    compact_prompt_saw_cleared_tool_result = true;
                }
            }
            return CompactEventProvider::chat(messages, tools);
        }
    };

    auto provider = std::make_shared<MicroProvider>();
    acecode::ToolExecutor tools;
    acecode::PermissionManager permissions;
    acecode::AgentCallbacks cb;

    acecode::AgentLoop loop(
        [&]() -> std::shared_ptr<acecode::LlmProvider> { return provider; },
        tools,
        cb,
        "/tmp/auto-micro-compact",
        permissions);
    loop.set_context_window(1);

    loop.push_message(loop_msg("user", "tool-producing turn"));
    loop.push_message(tool_call_msg("call-old", "bash"));
    loop.push_message(tool_result_msg("call-old", std::string(2000, 'x')));
    add_compactable_history(loop);

    auto events = wait_for_done(loop, [&] {
        loop.submit("trigger auto compact");
    });

    EXPECT_TRUE(provider->compact_prompt_saw_cleared_tool_result);
    bool saw_micro_message = false;
    for (const auto& evt : events) {
        if (evt.kind == acecode::SessionEventKind::Message &&
            evt.payload.value("role", "") == "system" &&
            evt.payload.value("content", "").find("[Micro-compact] Cleared") != std::string::npos) {
            saw_micro_message = true;
        }
    }
    EXPECT_TRUE(saw_micro_message);
}

TEST(AgentLoopCompactEvents, AutoCompactCircuitBreakerDoesNotBlockManualCompact) {
    auto provider = std::make_shared<CompactEventProvider>();
    provider->fail_compact = true;
    acecode::ToolExecutor tools;
    acecode::PermissionManager permissions;
    acecode::AgentCallbacks cb;

    acecode::AgentLoop loop(
        [&]() -> std::shared_ptr<acecode::LlmProvider> { return provider; },
        tools,
        cb,
        "/tmp/auto-compact-circuit",
        permissions);
    loop.set_context_window(1);
    add_compactable_history(loop, 8);

    for (int i = 0; i < 4; ++i) {
        wait_for_done(loop, [&] {
            loop.submit("trigger failing compact " + std::to_string(i));
        });
    }

    EXPECT_EQ(provider->chat_calls, acecode::MAX_CONSECUTIVE_AUTOCOMPACT_FAILURES)
        << "fourth auto-compact attempt should be skipped by the circuit breaker";

    wait_for_done(loop, [&] {
        loop.submit_compact();
    });
    EXPECT_EQ(provider->chat_calls, acecode::MAX_CONSECUTIVE_AUTOCOMPACT_FAILURES + 1)
        << "manual compact should still call the compact provider after auto circuit breaker trips";
}

TEST(AgentLoopCompactEvents, ContextOverflowRescueCompactsAndRetriesRequest) {
    auto provider = std::make_shared<acecode_test::StubLlmProvider>();
    provider->push_error(make_context_overflow_error());
    provider->push_text("recovered");
    acecode::ToolExecutor tools;
    acecode::PermissionManager permissions;
    acecode::AgentCallbacks cb;

    acecode::AgentLoop loop(
        [&]() -> std::shared_ptr<acecode::LlmProvider> { return provider; },
        tools,
        cb,
        "/tmp/rescue-compact",
        permissions);
    loop.set_context_window(1000000);
    add_compactable_history(loop, 6);

    auto events = wait_for_done(loop, [&] {
        loop.submit("latest user request");
    });

    EXPECT_EQ(provider->turn_count(), 2);
    auto first_request = provider->messages_for_turn(0);
    auto second_request = provider->messages_for_turn(1);
    EXPECT_LT(second_request.size(), first_request.size());
    EXPECT_TRUE(request_contains(second_request, "latest user request"));
    EXPECT_TRUE(request_contains(second_request, "detailed summary was not generated"));
    EXPECT_FALSE(request_contains(second_request, "old user 0"));

    bool saw_replace = false;
    bool saw_rescue_message = false;
    for (const auto& evt : events) {
        if (evt.kind == acecode::SessionEventKind::TranscriptReplace) {
            saw_replace = true;
        }
        if (evt.kind == acecode::SessionEventKind::Message &&
            evt.payload.value("role", "") == "system" &&
            evt.payload.value("content", "").find("[Rescue compact]") != std::string::npos) {
            saw_rescue_message = true;
        }
    }
    EXPECT_FALSE(saw_replace);
    EXPECT_TRUE(saw_rescue_message);
}

TEST(AgentLoopCompactEvents, ContextOverflowSingleTurnDoesNotRetryForever) {
    auto provider = std::make_shared<acecode_test::StubLlmProvider>();
    provider->push_error(make_context_overflow_error());
    acecode::ToolExecutor tools;
    acecode::PermissionManager permissions;
    acecode::AgentCallbacks cb;

    acecode::AgentLoop loop(
        [&]() -> std::shared_ptr<acecode::LlmProvider> { return provider; },
        tools,
        cb,
        "/tmp/rescue-compact",
        permissions);
    loop.set_context_window(1000000);

    auto events = wait_for_done(loop, [&] {
        loop.submit("single oversized turn");
    });

    EXPECT_EQ(provider->turn_count(), 1);
    bool saw_replace = false;
    bool saw_error = false;
    for (const auto& evt : events) {
        if (evt.kind == acecode::SessionEventKind::TranscriptReplace) {
            saw_replace = true;
        }
        if (evt.kind == acecode::SessionEventKind::Message &&
            evt.payload.value("role", "") == "error" &&
            evt.payload.value("content", "").find("cannot be rescued") != std::string::npos) {
            saw_error = true;
        }
    }
    EXPECT_FALSE(saw_replace);
    EXPECT_TRUE(saw_error);
}
