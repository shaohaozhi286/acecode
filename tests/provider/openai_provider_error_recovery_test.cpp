#include <gtest/gtest.h>

#include "provider/openai_provider.hpp"
#include "provider/llm_provider.hpp"

#include <httplib.h>

#include <atomic>
#include <chrono>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace std::chrono_literals;
using acecode::ChatMessage;
using acecode::OpenAiCompatProvider;
using acecode::ProviderErrorKind;
using acecode::StreamEvent;
using acecode::StreamEventType;
using acecode::ToolDef;

struct LocalHttpServer {
    httplib::Server svr;
    int port = 0;
    std::thread th;

    explicit LocalHttpServer(std::function<void(httplib::Server&)> setup) {
        setup(svr);
        port = svr.bind_to_any_port("127.0.0.1");
        th = std::thread([this] { svr.listen_after_bind(); });
        for (int i = 0; i < 50 && !svr.is_running(); ++i) {
            std::this_thread::sleep_for(10ms);
        }
    }

    ~LocalHttpServer() {
        svr.stop();
        if (th.joinable()) th.join();
    }
};

std::vector<ChatMessage> single_user_message() {
    ChatMessage msg;
    msg.role = "user";
    msg.content = "hello";
    return {msg};
}

std::vector<StreamEvent> collect_events(OpenAiCompatProvider& provider) {
    std::atomic<bool> abort_flag{false};
    std::vector<StreamEvent> events;
    std::mutex mu;
    provider.chat_stream(single_user_message(), std::vector<ToolDef>{},
                         [&](const StreamEvent& evt) {
                             std::lock_guard<std::mutex> lk(mu);
                             events.push_back(evt);
                         },
                         &abort_flag);
    return events;
}

const StreamEvent* last_error_event(const std::vector<StreamEvent>& events) {
    for (auto it = events.rbegin(); it != events.rend(); ++it) {
        if (it->type == StreamEventType::Error) return &*it;
    }
    return nullptr;
}

int count_events(const std::vector<StreamEvent>& events, StreamEventType type) {
    int count = 0;
    for (const auto& evt : events) {
        if (evt.type == type) ++count;
    }
    return count;
}

TEST(OpenAiProviderErrorRecovery, HttpJsonErrorBodyIsPreservedAndPrettyPrinted) {
    const std::string raw_body = R"({"unexpected":{"nested":true},"message":"bad request"})";
    LocalHttpServer server([&](httplib::Server& s) {
        s.Post("/chat/completions", [&](const httplib::Request&, httplib::Response& res) {
            res.status = 400;
            res.set_header("x-request-id", "req-json");
            res.set_content(raw_body, "application/json");
        });
    });

    OpenAiCompatProvider provider(
        "http://127.0.0.1:" + std::to_string(server.port), "", "test-model");

    const auto events = collect_events(provider);
    const StreamEvent* err = last_error_event(events);
    ASSERT_NE(err, nullptr);
    EXPECT_EQ(err->provider_error.kind, ProviderErrorKind::Http);
    EXPECT_EQ(err->provider_error.status_code, 400);
    EXPECT_EQ(err->provider_error.request_id, "req-json");
    EXPECT_TRUE(err->provider_error.body_is_json);
    EXPECT_EQ(err->provider_error.raw_body, raw_body);
    EXPECT_NE(err->provider_error.pretty_json.find("\"unexpected\": {"), std::string::npos);
    EXPECT_NE(err->error.find("\"nested\": true"), std::string::npos);
    EXPECT_EQ(count_events(events, StreamEventType::Done), 0);
}

TEST(OpenAiProviderErrorRecovery, HttpTextErrorBodyIsPreservedAsRawText) {
    const std::string raw_body = "gateway says nope";
    LocalHttpServer server([&](httplib::Server& s) {
        s.Post("/chat/completions", [&](const httplib::Request&, httplib::Response& res) {
            res.status = 400;
            res.set_content(raw_body, "text/plain");
        });
    });

    OpenAiCompatProvider provider(
        "http://127.0.0.1:" + std::to_string(server.port), "", "test-model");

    const auto events = collect_events(provider);
    const StreamEvent* err = last_error_event(events);
    ASSERT_NE(err, nullptr);
    EXPECT_EQ(err->provider_error.kind, ProviderErrorKind::Http);
    EXPECT_FALSE(err->provider_error.body_is_json);
    EXPECT_EQ(err->provider_error.raw_body, raw_body);
    EXPECT_NE(err->error.find(raw_body), std::string::npos);
}

TEST(OpenAiProviderErrorRecovery, NetworkFailureIsStructuredAndRetryable) {
    OpenAiCompatProvider provider("http://127.0.0.1:1", "", "test-model");

    const auto events = collect_events(provider);
    const StreamEvent* err = last_error_event(events);
    ASSERT_NE(err, nullptr);
    EXPECT_TRUE(err->provider_error.kind == ProviderErrorKind::Network ||
                err->provider_error.kind == ProviderErrorKind::Timeout);
    EXPECT_EQ(err->provider_error.status_code, 0);
    EXPECT_TRUE(err->provider_error.retryable);
    EXPECT_GE(count_events(events, StreamEventType::Retry), 1);
}

TEST(OpenAiProviderErrorRecovery, Http200NonSseJsonBodyBecomesMalformedSseError) {
    const std::string raw_body = R"({"error":{"message":"not an event stream"}})";
    LocalHttpServer server([&](httplib::Server& s) {
        s.Post("/chat/completions", [&](const httplib::Request&, httplib::Response& res) {
            res.status = 200;
            res.set_content(raw_body, "application/json");
        });
    });

    OpenAiCompatProvider provider(
        "http://127.0.0.1:" + std::to_string(server.port), "", "test-model");

    const auto events = collect_events(provider);
    const StreamEvent* err = last_error_event(events);
    ASSERT_NE(err, nullptr);
    EXPECT_EQ(err->provider_error.kind, ProviderErrorKind::MalformedSse);
    EXPECT_EQ(err->provider_error.status_code, 200);
    EXPECT_TRUE(err->provider_error.body_is_json);
    EXPECT_EQ(err->provider_error.raw_body, raw_body);
    EXPECT_NE(err->error.find("\"message\": \"not an event stream\""), std::string::npos);
}

TEST(OpenAiProviderErrorRecovery, Http200MalformedSseJsonBecomesMalformedJsonError) {
    LocalHttpServer server([&](httplib::Server& s) {
        s.Post("/chat/completions", [](const httplib::Request&, httplib::Response& res) {
            res.status = 200;
            res.set_content("data: {not-json}\n\n", "text/event-stream");
        });
    });

    OpenAiCompatProvider provider(
        "http://127.0.0.1:" + std::to_string(server.port), "", "test-model");

    const auto events = collect_events(provider);
    const StreamEvent* err = last_error_event(events);
    ASSERT_NE(err, nullptr);
    EXPECT_EQ(err->provider_error.kind, ProviderErrorKind::MalformedJson);
    EXPECT_EQ(err->provider_error.status_code, 200);
    EXPECT_FALSE(err->provider_error.body_is_json);
    EXPECT_NE(err->provider_error.raw_body.find("{not-json}"), std::string::npos);
}

TEST(OpenAiProviderErrorRecovery, RetriesTransientFailureBeforeAnyOutput) {
    std::atomic<int> calls{0};
    LocalHttpServer server([&](httplib::Server& s) {
        s.Post("/chat/completions", [&](const httplib::Request&, httplib::Response& res) {
            const int call = ++calls;
            if (call == 1) {
                res.status = 500;
                res.set_header("Retry-After", "0");
                res.set_content(R"({"error":"overloaded"})", "application/json");
                return;
            }
            res.status = 200;
            res.set_content(
                "data: {\"choices\":[{\"delta\":{\"content\":\"hi\"},\"finish_reason\":\"stop\"}]}\n\n"
                "data: [DONE]\n\n",
                "text/event-stream");
        });
    });

    OpenAiCompatProvider provider(
        "http://127.0.0.1:" + std::to_string(server.port), "", "test-model");

    const auto events = collect_events(provider);
    EXPECT_EQ(calls.load(), 2);
    EXPECT_EQ(count_events(events, StreamEventType::Retry), 1);
    EXPECT_EQ(count_events(events, StreamEventType::Error), 0);
    EXPECT_EQ(count_events(events, StreamEventType::Done), 1);
}

TEST(OpenAiProviderErrorRecovery, RetryExhaustionKeepsFinalProviderBodyVisible) {
    std::atomic<int> calls{0};
    const std::string raw_body = R"({"error":{"type":"overloaded_error","message":"try later"}})";
    LocalHttpServer server([&](httplib::Server& s) {
        s.Post("/chat/completions", [&](const httplib::Request&, httplib::Response& res) {
            ++calls;
            res.status = 500;
            res.set_header("Retry-After", "0");
            res.set_content(raw_body, "application/json");
        });
    });

    OpenAiCompatProvider provider(
        "http://127.0.0.1:" + std::to_string(server.port), "", "test-model");

    const auto events = collect_events(provider);
    const StreamEvent* err = last_error_event(events);
    ASSERT_NE(err, nullptr);
    EXPECT_EQ(calls.load(), 3);
    EXPECT_EQ(count_events(events, StreamEventType::Retry), 2);
    EXPECT_EQ(err->provider_error.kind, ProviderErrorKind::Http);
    EXPECT_EQ(err->provider_error.status_code, 500);
    EXPECT_TRUE(err->provider_error.body_is_json);
    EXPECT_EQ(err->provider_error.raw_body, raw_body);
    EXPECT_NE(err->error.find("\"overloaded_error\""), std::string::npos);
}

} // namespace
