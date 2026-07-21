// 覆盖 OpenAI-compatible provider 的自定义 request_headers 运行时行为。
//
// 关键契约:
//   - 非流式与流式请求都发送解析后的自定义 header。
//   - 自定义 Authorization 可以覆盖内置 Bearer api_key。
//   - {env:NAME} 每次请求解析;缺失环境变量时在发网前失败。

#include <gtest/gtest.h>

#include "provider/openai_provider.hpp"
#include "provider/llm_provider.hpp"

#include <httplib.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#ifndef _WIN32
#include <unistd.h>
#endif

namespace {

using namespace std::chrono_literals;
using acecode::ChatMessage;
using acecode::OpenAiCompatProvider;
using acecode::OpenAiConfig;
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

    std::string base_url() const {
        return "http://127.0.0.1:" + std::to_string(port);
    }
};

std::vector<ChatMessage> one_user_message() {
    ChatMessage msg;
    msg.role = "user";
    msg.content = "hi";
    return {msg};
}

std::optional<std::string> get_env_value(const char* name) {
    const char* value = std::getenv(name);
    if (!value) return std::nullopt;
    return std::string(value);
}

void set_env_value(const char* name, const std::optional<std::string>& value) {
#ifdef _WIN32
    _putenv_s(name, value ? value->c_str() : "");
#else
    if (value) {
        setenv(name, value->c_str(), 1);
    } else {
        unsetenv(name);
    }
#endif
}

struct ScopedEnv {
    std::string name;
    std::optional<std::string> old_value;

    ScopedEnv(const char* env_name, const std::optional<std::string>& value)
        : name(env_name), old_value(get_env_value(env_name)) {
        set_env_value(name.c_str(), value);
    }

    ~ScopedEnv() {
        set_env_value(name.c_str(), old_value);
    }
};

} // namespace

// 场景:非流式 chat 请求发送自定义 header,且 Authorization 模板覆盖内置 api_key。
TEST(OpenAiProviderRequestHeadersTest, NonStreamingUsesResolvedHeadersAndAuthorizationOverride) {
    ScopedEnv token("ACE_PROVIDER_HEADER_TOKEN", "env-token");
    std::mutex mu;
    std::string seen_team;
    std::string seen_token;
    std::string seen_authorization;

    LocalHttpServer server([&](httplib::Server& s) {
        s.Post("/chat/completions", [&](const httplib::Request& req, httplib::Response& res) {
            {
                std::lock_guard<std::mutex> lk(mu);
                seen_team = req.get_header_value("X-Team");
                seen_token = req.get_header_value("X-Token");
                seen_authorization = req.get_header_value("Authorization");
            }
            res.set_content(R"({"choices":[{"message":{"content":"ok"},"finish_reason":"stop"}]})",
                            "application/json");
        });
    });

    OpenAiCompatProvider provider(
        server.base_url(),
        "sk-original",
        "test-model",
        OpenAiConfig::kDefaultStreamTimeoutMs,
        {
            {"Authorization", "Bearer {env:ACE_PROVIDER_HEADER_TOKEN}"},
            {"X-Team", "acecode"},
            {"X-Token", "{env:ACE_PROVIDER_HEADER_TOKEN}"}
        });

    auto response = provider.chat(one_user_message(), {});
    EXPECT_EQ(response.content, "ok");

    std::lock_guard<std::mutex> lk(mu);
    EXPECT_EQ(seen_team, "acecode");
    EXPECT_EQ(seen_token, "env-token");
    EXPECT_EQ(seen_authorization, "Bearer env-token");
}

// 场景:流式 chat_stream 请求同样发送解析后的自定义 header。
TEST(OpenAiProviderRequestHeadersTest, StreamingUsesResolvedHeaders) {
    ScopedEnv token("ACE_PROVIDER_STREAM_HEADER", "stream-token");
    std::mutex mu;
    std::string seen_stream_header;

    LocalHttpServer server([&](httplib::Server& s) {
        s.Post("/chat/completions", [&](const httplib::Request& req, httplib::Response& res) {
            {
                std::lock_guard<std::mutex> lk(mu);
                seen_stream_header = req.get_header_value("X-Stream");
            }
            std::string body =
                "data: {\"choices\":[{\"delta\":{\"content\":\"ok\"},\"finish_reason\":\"stop\"}]}\n\n"
                "data: [DONE]\n\n";
            res.set_content(body, "text/event-stream");
        });
    });

    OpenAiCompatProvider provider(
        server.base_url(),
        "sk-original",
        "test-model",
        OpenAiConfig::kDefaultStreamTimeoutMs,
        {{"X-Stream", "{env:ACE_PROVIDER_STREAM_HEADER}"}});

    std::string aggregated;
    int done_events = 0;
    int error_events = 0;
    std::atomic<bool> abort_flag{false};
    provider.chat_stream(
        one_user_message(),
        std::vector<ToolDef>{},
        [&](const StreamEvent& evt) {
            if (evt.type == StreamEventType::Delta) aggregated += evt.content;
            if (evt.type == StreamEventType::Done) ++done_events;
            if (evt.type == StreamEventType::Error) ++error_events;
        },
        &abort_flag);

    EXPECT_EQ(aggregated, "ok");
    EXPECT_EQ(done_events, 1);
    EXPECT_EQ(error_events, 0);

    std::lock_guard<std::mutex> lk(mu);
    EXPECT_EQ(seen_stream_header, "stream-token");
}

// 场景:环境变量缺失时 provider 在发网前失败,不泄露或发送模板。
TEST(OpenAiProviderRequestHeadersTest, MissingEnvFailsBeforeNetwork) {
    ScopedEnv missing("ACE_PROVIDER_MISSING_HEADER", std::nullopt);
    std::atomic<int> hit_count{0};
    LocalHttpServer server([&](httplib::Server& s) {
        s.Post("/chat/completions", [&](const httplib::Request&, httplib::Response& res) {
            ++hit_count;
            res.set_content(R"({"choices":[{"message":{"content":"unexpected"}}]})",
                            "application/json");
        });
    });

    OpenAiCompatProvider provider(
        server.base_url(),
        "sk-original",
        "test-model",
        OpenAiConfig::kDefaultStreamTimeoutMs,
        {{"X-Token", "{env:ACE_PROVIDER_MISSING_HEADER}"}});

    auto response = provider.chat(one_user_message(), {});
    EXPECT_EQ(response.finish_reason, "error");
    EXPECT_NE(response.content.find("ACE_PROVIDER_MISSING_HEADER"), std::string::npos);
    EXPECT_TRUE(response.provider_error.has_error());
    EXPECT_FALSE(response.provider_error.retryable);
    EXPECT_EQ(hit_count.load(), 0);
}

TEST(OpenAiProviderRequestHeadersTest, NonStreamingHttpErrorIsStructured) {
    LocalHttpServer server([&](httplib::Server& s) {
        s.Post("/chat/completions", [&](const httplib::Request&, httplib::Response& res) {
            res.status = 429;
            res.set_header("x-request-id", "req-compact-retry");
            res.set_content(
                R"({"error":{"code":"rate_limit_exceeded","message":"slow down"}})",
                "application/json");
        });
    });
    OpenAiCompatProvider provider(
        server.base_url(), "sk-test", "test-model");

    auto response = provider.chat(one_user_message(), {});

    EXPECT_EQ(response.finish_reason, "error");
    EXPECT_EQ(response.provider_error.kind, acecode::ProviderErrorKind::Http);
    EXPECT_EQ(response.provider_error.status_code, 429);
    EXPECT_EQ(response.provider_error.request_id, "req-compact-retry");
    EXPECT_TRUE(response.provider_error.retryable);
    EXPECT_NE(response.provider_error.raw_body.find("rate_limit_exceeded"),
              std::string::npos);
}
