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

TEST(OpenAiProviderErrorRecovery, Http200SseErrorPayloadWithEmptyChoicesIsPrintedDirectly) {
    const std::string error_payload =
        R"({"error":"liteiim_AuthenticationError: AuthenticationError: OpenAIException - Error code: 401 - {'error': 'Unauthorized'}","choices":[]})";
    LocalHttpServer server([&](httplib::Server& s) {
        s.Post("/chat/completions", [&](const httplib::Request&, httplib::Response& res) {
            res.status = 200;
            res.set_content(
                "data: " + error_payload + "\n\n"
                "data: {\"usage\":{\"prompt_tokens\":0,\"completion_tokens\":0,\"total_tokens\":0},\"choices\":[]}\n\n"
                "data: [DONE]\n\n",
                "text/event-stream");
        });
    });

    OpenAiCompatProvider provider(
        "http://127.0.0.1:" + std::to_string(server.port), "", "test-model");

    const auto events = collect_events(provider);
    const StreamEvent* err = last_error_event(events);
    ASSERT_NE(err, nullptr);
    EXPECT_EQ(err->provider_error.kind, ProviderErrorKind::Http);
    EXPECT_EQ(err->provider_error.status_code, 401);
    EXPECT_TRUE(err->provider_error.body_is_json);
    EXPECT_EQ(err->provider_error.raw_body, error_payload);
    EXPECT_NE(err->error.find("liteiim_AuthenticationError"), std::string::npos);
    EXPECT_NE(err->error.find("Unauthorized"), std::string::npos);
    EXPECT_EQ(count_events(events, StreamEventType::Done), 0);
}

TEST(OpenAiProviderErrorRecovery, Http200PartialSseThenTransportTimeoutRetriesUntilSuccess) {
    std::atomic<int> calls{0};
    LocalHttpServer server([&](httplib::Server& s) {
        s.Post("/chat/completions", [&](const httplib::Request&, httplib::Response& res) {
            const int call = ++calls;
            res.status = 200;
            if (call > 1) {
                res.set_content(
                    "data: {\"choices\":[{\"delta\":{\"content\":\"ok\"},\"finish_reason\":\"stop\"}]}\n\n"
                    "data: [DONE]\n\n",
                    "text/event-stream");
                return;
            }
            res.set_chunked_content_provider(
                "text/event-stream",
                [sent = false](size_t, httplib::DataSink& sink) mutable {
                    if (!sent) {
                        sent = true;
                        const std::string chunk =
                            "data: {\"choices\":[{\"delta\":{\"content\":\"hi\"}}]}\n\n";
                        sink.write(chunk.data(), chunk.size());
                    }
                    std::this_thread::sleep_for(500ms);
                    return false;
                });
        });
    });

    OpenAiCompatProvider provider(
        "http://127.0.0.1:" + std::to_string(server.port), "", "test-model", 150);

    const auto events = collect_events(provider);
    EXPECT_EQ(calls.load(), 2);
    EXPECT_EQ(last_error_event(events), nullptr);
    EXPECT_EQ(count_events(events, StreamEventType::Retry), 1);
    EXPECT_EQ(count_events(events, StreamEventType::Done), 1);
    EXPECT_GE(count_events(events, StreamEventType::Delta), 2);
}

TEST(OpenAiProviderErrorRecovery, TimeoutBeforeAnySseDataRetriesUntilSuccess) {
    std::atomic<int> calls{0};
    LocalHttpServer server([&](httplib::Server& s) {
        s.Post("/chat/completions", [&](const httplib::Request&, httplib::Response& res) {
            const int call = ++calls;
            res.status = 200;
            if (call < 3) {
                std::this_thread::sleep_for(300ms);
                res.set_content("data: [DONE]\n\n", "text/event-stream");
                return;
            }
            res.set_content(
                "data: {\"choices\":[{\"delta\":{\"content\":\"ok\"},\"finish_reason\":\"stop\"}]}\n\n"
                "data: [DONE]\n\n",
                "text/event-stream");
        });
    });

    OpenAiCompatProvider provider(
        "http://127.0.0.1:" + std::to_string(server.port), "", "test-model", 100);

    const auto events = collect_events(provider);
    EXPECT_EQ(calls.load(), 3);
    EXPECT_EQ(last_error_event(events), nullptr);
    EXPECT_EQ(count_events(events, StreamEventType::Retry), 2);
    EXPECT_EQ(count_events(events, StreamEventType::Done), 1);
}

TEST(OpenAiProviderErrorRecovery, RepeatedTimeoutRetryStopsOnUserAbort) {
    std::atomic<int> calls{0};
    LocalHttpServer server([&](httplib::Server& s) {
        s.Post("/chat/completions", [&](const httplib::Request&, httplib::Response& res) {
            ++calls;
            std::this_thread::sleep_for(300ms);
            res.status = 200;
            res.set_content("data: [DONE]\n\n", "text/event-stream");
        });
    });

    OpenAiCompatProvider provider(
        "http://127.0.0.1:" + std::to_string(server.port), "", "test-model", 100);

    std::atomic<bool> abort_flag{false};
    std::atomic<int> retry_events{0};
    std::vector<StreamEvent> events;
    std::mutex mu;
    provider.chat_stream(single_user_message(), std::vector<ToolDef>{},
                         [&](const StreamEvent& evt) {
                             {
                                 std::lock_guard<std::mutex> lk(mu);
                                 events.push_back(evt);
                             }
                             if (evt.type == StreamEventType::Retry &&
                                 ++retry_events >= 2) {
                                 abort_flag.store(true);
                             }
                         },
                         &abort_flag);

    const StreamEvent* err = last_error_event(events);
    ASSERT_NE(err, nullptr);
    EXPECT_EQ(err->provider_error.kind, ProviderErrorKind::UserCancelled);
    EXPECT_EQ(retry_events.load(), 2);
    EXPECT_EQ(count_events(events, StreamEventType::Done), 0);
}

TEST(OpenAiProviderErrorRecovery, IncompleteSseWithoutTransportTimeoutNowRetriesThenFailsMalformedSse) {
    // 触发场景:服务端发出一段 content delta 后正常关闭 HTTP 连接,但从未发送
    // [DONE] —— 也没有触发 transport timeout(libcurl 不会报错)。这是不稳定
    // 私有模型最常见的"跑着跑着停了"形态。
    // 期望行为:provider 把 MalformedSse 标为 retryable(因为没有已闭合的
    // tool_call),走 drop-partial 重试族,达到 kStreamMaxAttempts(3) 次仍失败
    // 后才报错。
    // 回归测试:此前 IncompleteSseWithoutTransportTimeoutStaysMalformedSse 测
    // 试断言 retry=0、calls=1、立即报错 —— 那是"中途断流直接放弃整个 turn"
    // 的老行为,对脆弱私有模型极不友好,现已改为下面的新断言。
    std::atomic<int> calls{0};
    LocalHttpServer server([&](httplib::Server& s) {
        s.Post("/chat/completions", [&](const httplib::Request&, httplib::Response& res) {
            ++calls;
            res.status = 200;
            res.set_content(
                "data: {\"choices\":[{\"delta\":{\"content\":\"hi\"}}]}\n\n",
                "text/event-stream");
        });
    });

    OpenAiCompatProvider provider(
        "http://127.0.0.1:" + std::to_string(server.port), "", "test-model", 1000);

    const auto events = collect_events(provider);
    const StreamEvent* err = last_error_event(events);
    ASSERT_NE(err, nullptr);
    EXPECT_EQ(calls.load(), 3);
    EXPECT_EQ(err->provider_error.kind, ProviderErrorKind::MalformedSse);
    EXPECT_EQ(err->provider_error.status_code, 200);
    EXPECT_TRUE(err->provider_error.retryable);
    // 每次 attempt 都重新流出一个 content delta(总共 3 次),因为 partial
    // 在 provider 端被丢弃了 —— 但 callback 早就吃过 delta,这里仅做计数。
    EXPECT_EQ(count_events(events, StreamEventType::Delta), 3);
    EXPECT_EQ(count_events(events, StreamEventType::Retry), 2);
    EXPECT_EQ(count_events(events, StreamEventType::Done), 0);
}

TEST(OpenAiProviderErrorRecovery, FinishReasonStopWithoutDoneNowRetriesThenFailsMalformedSse) {
    // 触发场景:服务端发了 content delta 且带 finish_reason=stop,但没有 [DONE]
    // 帧。同样属于 SSE 协议未完成 —— 没有已闭合 tool_call,应该走 drop-partial 重试。
    // 回归测试:此前 FinishReasonStopWithoutDoneDoesNotCompleteStream 断言 retry=0、
    // 立即报错;现在期望走重试族直到 kStreamMaxAttempts 用完。
    std::atomic<int> calls{0};
    LocalHttpServer server([&](httplib::Server& s) {
        s.Post("/chat/completions", [&](const httplib::Request&, httplib::Response& res) {
            ++calls;
            res.status = 200;
            res.set_content(
                "data: {\"choices\":[{\"delta\":{\"content\":\"hi\"},\"finish_reason\":\"stop\"}]}\n\n",
                "text/event-stream");
        });
    });

    OpenAiCompatProvider provider(
        "http://127.0.0.1:" + std::to_string(server.port), "", "test-model", 1000);

    const auto events = collect_events(provider);
    const StreamEvent* err = last_error_event(events);
    ASSERT_NE(err, nullptr);
    EXPECT_EQ(calls.load(), 3);
    EXPECT_EQ(err->provider_error.kind, ProviderErrorKind::MalformedSse);
    EXPECT_TRUE(err->provider_error.retryable);
    EXPECT_EQ(count_events(events, StreamEventType::Delta), 3);
    EXPECT_EQ(count_events(events, StreamEventType::Retry), 2);
    EXPECT_EQ(count_events(events, StreamEventType::Done), 0);
}

TEST(OpenAiProviderErrorRecovery, Http200MalformedSseRecoversOnRetry) {
    // 触发场景:第一次返回 content delta 后不发 [DONE](连接关闭),第二次返
    // 回完整 SSE 流。这正是用户公司自建模型频繁出现的"跑着跑着断了"的可
    // 恢复形态。
    // 期望行为:provider 在第一次失败时 emit Retry,第二次成功 emit Done,
    // 整个 stream 视为成功;无 Error 事件外发,LLM 拿到完整回复。
    // 非显然阈值:Delta 至少 2 次 —— 第一次 partial(被 AgentLoop 丢弃)+ 第
    // 二次正常 stream 的 1 个或多个 delta。
    std::atomic<int> calls{0};
    LocalHttpServer server([&](httplib::Server& s) {
        s.Post("/chat/completions", [&](const httplib::Request&, httplib::Response& res) {
            const int call = ++calls;
            res.status = 200;
            if (call == 1) {
                res.set_content(
                    "data: {\"choices\":[{\"delta\":{\"content\":\"hi\"}}]}\n\n",
                    "text/event-stream");
                return;
            }
            res.set_content(
                "data: {\"choices\":[{\"delta\":{\"content\":\"ok\"},\"finish_reason\":\"stop\"}]}\n\n"
                "data: [DONE]\n\n",
                "text/event-stream");
        });
    });

    OpenAiCompatProvider provider(
        "http://127.0.0.1:" + std::to_string(server.port), "", "test-model", 1000);

    const auto events = collect_events(provider);
    EXPECT_EQ(calls.load(), 2);
    EXPECT_EQ(last_error_event(events), nullptr);
    EXPECT_EQ(count_events(events, StreamEventType::Retry), 1);
    EXPECT_EQ(count_events(events, StreamEventType::Done), 1);
    EXPECT_GE(count_events(events, StreamEventType::Delta), 2);
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
