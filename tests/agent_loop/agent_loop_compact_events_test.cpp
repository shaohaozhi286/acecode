#include <gtest/gtest.h>

#include "agent_loop.hpp"
#include "permissions.hpp"
#include "provider/llm_provider.hpp"
#include "tool/tool_executor.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>

using namespace std::chrono_literals;

namespace {

class CompactEventProvider : public acecode::LlmProvider {
public:
    acecode::ChatResponse chat(const std::vector<acecode::ChatMessage>&,
                               const std::vector<acecode::ToolDef>&) override {
        acecode::ChatResponse resp;
        resp.content = "<summary>Compacted event summary.</summary>";
        resp.finish_reason = "stop";
        return resp;
    }

    void chat_stream(const std::vector<acecode::ChatMessage>&,
                     const std::vector<acecode::ToolDef>&,
                     const acecode::StreamCallback&,
                     std::atomic<bool>* = nullptr) override {}

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

} // namespace

TEST(AgentLoopCompactEvents, QueuedCompactEmitsTranscriptReplacement) {
    auto provider = std::make_shared<CompactEventProvider>();
    acecode::ToolExecutor tools;
    acecode::PermissionManager permissions;

    std::mutex mu;
    std::condition_variable cv;
    bool done = false;

    acecode::AgentCallbacks cb;
    cb.on_busy_changed = [&](bool busy) {
        if (!busy) {
            std::lock_guard<std::mutex> lk(mu);
            done = true;
            cv.notify_all();
        }
    };

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

    bool saw_replace = false;
    bool saw_completion = false;
    for (const auto& evt : events) {
        if (evt.kind == acecode::SessionEventKind::TranscriptReplace) {
            saw_replace = true;
            ASSERT_TRUE(evt.payload.contains("messages"));
            ASSERT_TRUE(evt.payload["messages"].is_array());
            EXPECT_GE(evt.payload["messages"].size(), 4u);
            EXPECT_EQ(evt.payload["messages_compressed"], 2);
        }
        if (evt.kind == acecode::SessionEventKind::Message &&
            evt.payload.value("role", "") == "system" &&
            evt.payload.value("content", "").find("Compacted 2 messages") != std::string::npos) {
            saw_completion = true;
        }
    }

    EXPECT_TRUE(saw_replace);
    EXPECT_TRUE(saw_completion);
    loop.events().unsubscribe(sub);
}
