#include <gtest/gtest.h>

#include "agent_loop.hpp"
#include "permissions.hpp"
#include "session/session_manager.hpp"
#include "session/session_storage.hpp"
#include "stub_provider.hpp"
#include "tool/tool_executor.hpp"

#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

using namespace std::chrono_literals;

namespace fs = std::filesystem;

namespace {

fs::path steering_temp_cwd(const std::string& hint) {
    auto dir = fs::temp_directory_path() /
        ("acecode_turn_steering_" + hint + "_" +
         std::to_string(std::random_device{}()));
    fs::remove_all(dir);
    fs::create_directories(dir);
    fs::remove_all(acecode::SessionStorage::get_project_dir(dir.string()));
    return dir;
}

class TurnSteeringHarness {
public:
    explicit TurnSteeringHarness(const std::string& hint)
        : cwd_(steering_temp_cwd(hint)) {
        sm_.start_session(cwd_.string(), "stub", "stub-1", "sid-" + hint);

        acecode::AgentCallbacks callbacks;
        callbacks.on_busy_changed = [this](bool busy) {
            std::lock_guard<std::mutex> lk(mu_);
            busy_ = busy;
            saw_busy_ = saw_busy_ || busy;
            cv_.notify_all();
        };

        auto accessor = [this]() -> std::shared_ptr<acecode::LlmProvider> {
            return provider_;
        };
        loop_ = std::make_unique<acecode::AgentLoop>(
            accessor, tools_, callbacks, cwd_.string(), permissions_);
        loop_->set_session_manager(&sm_);
        sub_ = loop_->events().subscribe([this](const acecode::SessionEvent& event) {
            std::lock_guard<std::mutex> lk(events_mu_);
            events_.push_back(event);
        });
    }

    ~TurnSteeringHarness() {
        if (loop_ && sub_ != 0) loop_->events().unsubscribe(sub_);
        loop_.reset();
        fs::remove_all(cwd_);
        fs::remove_all(acecode::SessionStorage::get_project_dir(cwd_.string()));
    }

    acecode::AgentLoop& loop() { return *loop_; }
    acecode_test::StubLlmProvider& provider() { return *provider_; }
    acecode::SessionManager& session_manager() { return sm_; }

    std::string wait_for_active_turn(std::chrono::milliseconds timeout = 5s) {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline) {
            const std::string id = loop_->active_turn_id();
            if (!id.empty()) return id;
            std::this_thread::sleep_for(2ms);
        }
        return loop_->active_turn_id();
    }

    bool wait_until_idle(std::chrono::milliseconds timeout = 10s) {
        std::unique_lock<std::mutex> lk(mu_);
        return cv_.wait_for(lk, timeout, [this] {
            return saw_busy_ && !busy_;
        });
    }

    std::vector<acecode::SessionEvent> events() const {
        std::lock_guard<std::mutex> lk(events_mu_);
        return events_;
    }

private:
    fs::path cwd_;
    std::shared_ptr<acecode_test::StubLlmProvider> provider_ =
        std::make_shared<acecode_test::StubLlmProvider>();
    acecode::ToolExecutor tools_;
    acecode::PermissionManager permissions_;
    acecode::SessionManager sm_;
    std::unique_ptr<acecode::AgentLoop> loop_;
    acecode::EventDispatcher::SubscriptionId sub_ = 0;

    mutable std::mutex mu_;
    std::condition_variable cv_;
    bool busy_ = false;
    bool saw_busy_ = false;

    mutable std::mutex events_mu_;
    std::vector<acecode::SessionEvent> events_;
};

std::vector<std::string> request_user_texts(
    const std::vector<acecode::ChatMessage>& messages) {
    std::vector<std::string> texts;
    for (const auto& message : messages) {
        if (message.role == "user") texts.push_back(message.content);
    }
    return texts;
}

} // namespace

TEST(AgentLoopTurnSteering, RejectsInvalidIdleAndMismatchedRequests) {
    TurnSteeringHarness h("reject");

    acecode::UserInput guidance;
    guidance.text = "adjust";
    EXPECT_EQ(
        h.loop().steer_input("idle-turn", guidance).status,
        acecode::TurnSteerStatus::NoActiveTurn);

    acecode::UserInput blank;
    blank.text = " \t\r\n";
    EXPECT_EQ(
        h.loop().steer_input("idle-turn", blank).status,
        acecode::TurnSteerStatus::InvalidInput);

    h.provider().set_latency_ms(120);
    h.provider().push_text("first answer");
    h.loop().submit("start");
    const std::string turn_id = h.wait_for_active_turn();
    ASSERT_FALSE(turn_id.empty());

    auto mismatch = h.loop().steer_input("another-turn", guidance);
    EXPECT_EQ(mismatch.status, acecode::TurnSteerStatus::TurnMismatch);
    EXPECT_EQ(mismatch.turn_id, turn_id);

    ASSERT_TRUE(h.wait_until_idle());
    EXPECT_TRUE(h.loop().active_turn_id().empty());
}

TEST(AgentLoopTurnSteering, CommitsStructuredInputsInFifoAndContinuesSameTurn) {
    TurnSteeringHarness h("fifo");
    h.provider().set_latency_ms(100);
    h.provider().push_text("intermediate");
    h.provider().push_text("final");

    h.loop().submit("start");
    const std::string turn_id = h.wait_for_active_turn();
    ASSERT_FALSE(turn_id.empty());

    acecode::UserInput first;
    first.text = "first guidance";
    first.metadata["client_message_id"] = "guide-1";

    acecode::UserInput second;
    second.text = "second guidance";
    second.display_text = "second visible guidance";
    second.content_parts = nlohmann::json::array({
        nlohmann::json{{"type", "text"}, {"text", "second guidance"}},
        nlohmann::json{{"type", "browser_context"},
                       {"context", nlohmann::json{{"url", "https://example.test"}}}},
    });
    second.metadata["client_message_id"] = "guide-2";

    EXPECT_TRUE(h.loop().steer_input(turn_id, first).accepted());
    EXPECT_TRUE(h.loop().steer_input(turn_id, second).accepted());
    ASSERT_TRUE(h.wait_until_idle());

    ASSERT_EQ(h.provider().turn_count(), 2);
    const auto user_texts = request_user_texts(h.provider().messages_for_turn(1));
    ASSERT_GE(user_texts.size(), 3u);
    EXPECT_NE(
        user_texts[user_texts.size() - 3].find("start"),
        std::string::npos);
    EXPECT_NE(
        user_texts[user_texts.size() - 2].find("first guidance"),
        std::string::npos);
    EXPECT_NE(
        user_texts[user_texts.size() - 1].find("second guidance"),
        std::string::npos);

    std::vector<acecode::ChatMessage> guided;
    for (const auto& message : h.loop().messages()) {
        if (message.metadata.is_object() &&
            message.metadata.value("turn_steer", false)) {
            guided.push_back(message);
        }
    }
    ASSERT_EQ(guided.size(), 2u);
    EXPECT_EQ(guided[0].metadata.value("client_message_id", ""), "guide-1");
    EXPECT_EQ(guided[1].metadata.value("client_message_id", ""), "guide-2");
    EXPECT_EQ(guided[1].metadata.value("display_text", ""),
              "second visible guidance");
    EXPECT_EQ(guided[1].metadata.value("turn_id", ""), turn_id);
    EXPECT_TRUE(guided[1].content_parts.is_array());
    EXPECT_EQ(guided[1].content_parts.size(), 2u);

    const auto persisted = h.session_manager().load_active_messages();
    int persisted_guidance = 0;
    for (const auto& message : persisted) {
        if (message.metadata.is_object() &&
            message.metadata.value("turn_steer", false)) {
            ++persisted_guidance;
        }
    }
    EXPECT_EQ(persisted_guidance, 2);

    bool saw_started_turn_id = false;
    bool saw_committed_client_id = false;
    for (const auto& event : h.events()) {
        if (event.kind == acecode::SessionEventKind::BusyChanged &&
            event.payload.value("busy", false) &&
            event.payload.value("turn_id", "") == turn_id) {
            saw_started_turn_id = true;
        }
        if (event.kind == acecode::SessionEventKind::Message &&
            event.payload.value("role", "") == "user" &&
            event.payload.contains("metadata") &&
            event.payload["metadata"].value("client_message_id", "") == "guide-2") {
            saw_committed_client_id = true;
        }
    }
    EXPECT_TRUE(saw_started_turn_id);
    EXPECT_TRUE(saw_committed_client_id);
}

TEST(AgentLoopTurnSteering, EveryAcceptedFinalBoundaryRaceInputIsCommitted) {
    TurnSteeringHarness h("race");
    h.provider().set_latency_ms(20);
    for (int i = 0; i < 16; ++i) {
        h.provider().push_text("response-" + std::to_string(i));
    }

    h.loop().submit("start");
    const std::string turn_id = h.wait_for_active_turn();
    ASSERT_FALSE(turn_id.empty());

    std::vector<std::string> accepted_ids;
    for (int i = 0; i < 80; ++i) {
        acecode::UserInput guidance;
        guidance.text = "race guidance " + std::to_string(i);
        const std::string client_id = "race-" + std::to_string(i);
        guidance.metadata["client_message_id"] = client_id;
        const auto result = h.loop().steer_input(turn_id, guidance);
        if (result.accepted()) {
            accepted_ids.push_back(client_id);
        } else if (result.status == acecode::TurnSteerStatus::NoActiveTurn ||
                   result.status == acecode::TurnSteerStatus::NonSteerable) {
            break;
        } else {
            FAIL() << "unexpected steer status: "
                   << acecode::to_string(result.status);
        }
        std::this_thread::sleep_for(1ms);
    }

    ASSERT_TRUE(h.wait_until_idle());
    ASSERT_FALSE(accepted_ids.empty());

    std::vector<std::string> committed_ids;
    for (const auto& message : h.loop().messages()) {
        if (!message.metadata.is_object() ||
            !message.metadata.value("turn_steer", false)) {
            continue;
        }
        committed_ids.push_back(
            message.metadata.value("client_message_id", ""));
    }
    EXPECT_EQ(committed_ids, accepted_ids);
}
