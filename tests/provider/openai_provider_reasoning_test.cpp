// 覆盖 src/provider/openai_provider.cpp 中 reasoning_content 的读写双向支持。
//
// 背景：DeepSeek thinking 模式（V3.1 / V3.2 / V4）在 SSE 流里会同时返回
//   delta.content（最终答案）与 delta.reasoning_content（思维链），并且
//   要求把上一轮 assistant 的 reasoning_content 在下一次 API 请求里回传，
//   否则报 400 "The reasoning_content in the thinking mode must be passed
//   back to the API"。OpenRouter / Qwen / Moonshot 用 `reasoning` 作为别名。
//
// 见 openspec/changes/support-deepseek-reasoning。
//
// 本文件覆盖的场景：
//   1. SSE 流里 delta.reasoning_content 累积成 ChatResponse.reasoning_content
//      并触发 StreamEventType::ReasoningDelta 事件（DeepSeek 主路径）。
//   2. SSE 流里 delta.reasoning（OpenRouter / Qwen 别名）也能正确累积。
//   3. 非流式 parse_response 同样支持 reasoning_content / reasoning 两种字段。
//   4. build_request_body 在 assistant 消息上回传 reasoning_content，
//      其它角色（user / system / tool）以及空字段时不发该字段。
//
// 注：build_request_body 和 parse_response 是 protected 方法，这里通过
// 派生测试访问类 `TestableProvider` 暴露给测试用例。

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
using acecode::ChatResponse;
using acecode::OpenAiCompatProvider;
using acecode::StreamEvent;
using acecode::StreamEventType;
using acecode::ToolDef;

// 派生类：把 protected 的 build_request_body / parse_response 暴露成 public，
// 便于在不发起真实 HTTP 请求的情况下针对纯函数行为做断言。
class TestableProvider : public OpenAiCompatProvider {
public:
    using OpenAiCompatProvider::OpenAiCompatProvider;
    using OpenAiCompatProvider::build_request_body;
    using OpenAiCompatProvider::parse_response;
};

// 启动一个本地 httplib server，用于流式用例。析构时自动 stop。
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

// 用例 1：SSE 流里 delta.reasoning_content（DeepSeek 主名）能被解析出来。
//
// 期望：accumulated.reasoning_content 应等于片段拼接结果，并且每个非空片段
// 触发一次 StreamEventType::ReasoningDelta 事件，content 字段保留原样不被
// 当作普通 content 处理。
TEST(OpenAiProviderReasoningTest, SseAccumulatesDeepSeekReasoningContent) {
    LocalHttpServer server([](httplib::Server& s) {
        s.Post("/chat/completions", [](const httplib::Request&, httplib::Response& res) {
            // 两个 reasoning 片段 + 一个普通 content + DONE。
            std::string body =
                "data: {\"choices\":[{\"delta\":{\"reasoning_content\":\"step 1\"}}]}\n\n"
                "data: {\"choices\":[{\"delta\":{\"reasoning_content\":\" step 2\"}}]}\n\n"
                "data: {\"choices\":[{\"delta\":{\"content\":\"final\"},\"finish_reason\":\"stop\"}]}\n\n"
                "data: [DONE]\n\n";
            res.set_content(body, "text/event-stream");
            res.status = 200;
        });
    });

    OpenAiCompatProvider provider(
        "http://127.0.0.1:" + std::to_string(server.port), "", "test-model");

    ChatMessage user_msg;
    user_msg.role = "user";
    user_msg.content = "hello";
    std::vector<ChatMessage> messages = {user_msg};
    std::vector<ToolDef> tools;

    std::vector<StreamEvent> events;
    std::mutex evt_mu;
    std::string aggregated_reasoning;
    std::string aggregated_content;
    auto cb = [&](const StreamEvent& evt) {
        std::lock_guard<std::mutex> lk(evt_mu);
        events.push_back(evt);
        if (evt.type == StreamEventType::ReasoningDelta) aggregated_reasoning += evt.content;
        if (evt.type == StreamEventType::Delta) aggregated_content += evt.content;
    };

    std::atomic<bool> abort_flag{false};
    provider.chat_stream(messages, tools, cb, &abort_flag);

    // 拼接结果断言（顺序敏感）。
    EXPECT_EQ(aggregated_reasoning, "step 1 step 2");
    EXPECT_EQ(aggregated_content, "final");

    // ReasoningDelta 应触发恰好两次，普通 Delta 触发恰好一次。
    int reasoning_events = 0, delta_events = 0, done_events = 0, error_events = 0;
    {
        std::lock_guard<std::mutex> lk(evt_mu);
        for (const auto& evt : events) {
            switch (evt.type) {
                case StreamEventType::ReasoningDelta: ++reasoning_events; break;
                case StreamEventType::Delta:          ++delta_events; break;
                case StreamEventType::Done:           ++done_events; break;
                case StreamEventType::Error:          ++error_events; break;
                default: break;
            }
        }
    }
    EXPECT_EQ(reasoning_events, 2);
    EXPECT_EQ(delta_events, 1);
    EXPECT_EQ(done_events, 1);
    EXPECT_EQ(error_events, 0);
}

// 用例 2：SSE 流里 delta.reasoning（OpenRouter / Qwen 别名）也能被识别。
//
// 当 chunk 里没有 reasoning_content 字段，只有 reasoning 字段时，应当走
// 后备分支累积到同一个 ChatResponse.reasoning_content。
TEST(OpenAiProviderReasoningTest, SseRecognizesOpenRouterReasoningAlias) {
    LocalHttpServer server([](httplib::Server& s) {
        s.Post("/chat/completions", [](const httplib::Request&, httplib::Response& res) {
            std::string body =
                "data: {\"choices\":[{\"delta\":{\"reasoning\":\"alias chunk\"}}]}\n\n"
                "data: {\"choices\":[{\"delta\":{\"content\":\"ok\"},\"finish_reason\":\"stop\"}]}\n\n"
                "data: [DONE]\n\n";
            res.set_content(body, "text/event-stream");
            res.status = 200;
        });
    });

    OpenAiCompatProvider provider(
        "http://127.0.0.1:" + std::to_string(server.port), "", "test-model");
    ChatMessage user_msg;
    user_msg.role = "user";
    user_msg.content = "hi";
    std::vector<ChatMessage> messages = {user_msg};
    std::vector<ToolDef> tools;

    std::string aggregated_reasoning;
    int reasoning_events = 0;
    auto cb = [&](const StreamEvent& evt) {
        if (evt.type == StreamEventType::ReasoningDelta) {
            aggregated_reasoning += evt.content;
            ++reasoning_events;
        }
    };

    std::atomic<bool> abort_flag{false};
    provider.chat_stream(messages, tools, cb, &abort_flag);

    EXPECT_EQ(aggregated_reasoning, "alias chunk");
    EXPECT_EQ(reasoning_events, 1);
}

// 用例 3：非流式 parse_response 接住两种字段名。
//
// 优先 reasoning_content；缺失时回退 reasoning；两个都缺时为空字符串。
TEST(OpenAiProviderReasoningTest, ParseResponseHandlesBothFieldNames) {
    auto j_primary = nlohmann::json::parse(R"({
        "choices":[{
            "message":{"role":"assistant","content":"answer","reasoning_content":"deep think"},
            "finish_reason":"stop"
        }]
    })");
    auto resp_primary = TestableProvider::parse_response(j_primary);
    EXPECT_EQ(resp_primary.content, "answer");
    EXPECT_EQ(resp_primary.reasoning_content, "deep think");

    auto j_alias = nlohmann::json::parse(R"({
        "choices":[{
            "message":{"role":"assistant","content":"answer","reasoning":"alias think"},
            "finish_reason":"stop"
        }]
    })");
    auto resp_alias = TestableProvider::parse_response(j_alias);
    EXPECT_EQ(resp_alias.content, "answer");
    EXPECT_EQ(resp_alias.reasoning_content, "alias think");

    auto j_none = nlohmann::json::parse(R"({
        "choices":[{
            "message":{"role":"assistant","content":"answer"},
            "finish_reason":"stop"
        }]
    })");
    auto resp_none = TestableProvider::parse_response(j_none);
    EXPECT_EQ(resp_none.content, "answer");
    EXPECT_EQ(resp_none.reasoning_content, "");
}

// 用例 4：build_request_body 在 assistant 消息上回传非空 reasoning_content。
//
// 这里不发起真实请求，只检查序列化后的 JSON 字段。这是 DeepSeek 的硬性
// 协议要求 —— 缺这个字段会立即 400。
TEST(OpenAiProviderReasoningTest, BuildRequestBodyEchoesReasoningOnAssistant) {
    TestableProvider provider("http://example.invalid", "", "test-model");

    ChatMessage user_msg;
    user_msg.role = "user";
    user_msg.content = "do work";

    ChatMessage assistant_msg;
    assistant_msg.role = "assistant";
    assistant_msg.content = "ok";
    assistant_msg.reasoning_content = "I should do A then B";

    std::vector<ChatMessage> messages = {user_msg, assistant_msg};
    std::vector<ToolDef> tools;

    auto body = provider.build_request_body(messages, tools, /*stream=*/false);
    ASSERT_TRUE(body.contains("messages"));
    const auto& msgs = body["messages"];
    ASSERT_EQ(msgs.size(), 2u);

    // user 消息绝对不应携带 reasoning_content。
    EXPECT_FALSE(msgs[0].contains("reasoning_content"));

    // assistant 消息必须把 reasoning_content 回传。
    ASSERT_TRUE(msgs[1].contains("reasoning_content"));
    EXPECT_EQ(msgs[1]["reasoning_content"].get<std::string>(), "I should do A then B");
}

// 用例 5：build_request_body 在字段为空时不发该 key（兼容非 reasoning 模型）。
//
// OpenAI、Copilot、本地 LMStudio 等不识别 reasoning_content 的服务在某些
// 实现下可能拒绝未知字段；只在非空时附带，零开销保留兼容性。
TEST(OpenAiProviderReasoningTest, BuildRequestBodyOmitsEmptyReasoning) {
    TestableProvider provider("http://example.invalid", "", "test-model");

    ChatMessage assistant_msg;
    assistant_msg.role = "assistant";
    assistant_msg.content = "plain reply";
    // reasoning_content 默认为 ""

    ChatMessage tool_msg;
    tool_msg.role = "tool";
    tool_msg.content = "tool output";
    tool_msg.tool_call_id = "call_1";
    // 即使误填也不应出现在请求体里
    tool_msg.reasoning_content = "should be ignored on tool role";

    std::vector<ChatMessage> messages = {assistant_msg, tool_msg};
    std::vector<ToolDef> tools;

    auto body = provider.build_request_body(messages, tools, /*stream=*/false);
    const auto& msgs = body["messages"];
    ASSERT_EQ(msgs.size(), 2u);

    // assistant 消息：reasoning_content 为空 -> 不发字段
    EXPECT_FALSE(msgs[0].contains("reasoning_content"));

    // tool 消息：即使内部字段非空，也不发 (角色不是 assistant)
    EXPECT_FALSE(msgs[1].contains("reasoning_content"));
}

// 用例 6：build_request_body 在 assistant + tool_calls 组合下也正确回传。
//
// DeepSeek thinking 模式最常见的失败路径就是 "assistant 调用工具的那一轮也带了
// reasoning_content"，下一轮如果不回传就 400。这个用例守住该回归。
TEST(OpenAiProviderReasoningTest, BuildRequestBodyEchoesReasoningWithToolCalls) {
    TestableProvider provider("http://example.invalid", "", "test-model");

    ChatMessage assistant_msg;
    assistant_msg.role = "assistant";
    assistant_msg.content = ""; // tool-call 轮次允许 content 为空
    assistant_msg.reasoning_content = "decide to call file_read";
    assistant_msg.tool_calls = nlohmann::json::array({
        nlohmann::json{
            {"id", "call_1"},
            {"type", "function"},
            {"function", {
                {"name", "file_read"},
                {"arguments", "{\"file_path\":\"a.md\"}"}
            }}
        }
    });

    std::vector<ChatMessage> messages = {assistant_msg};
    std::vector<ToolDef> tools;

    auto body = provider.build_request_body(messages, tools, false);
    const auto& msgs = body["messages"];
    ASSERT_EQ(msgs.size(), 1u);
    ASSERT_TRUE(msgs[0].contains("tool_calls"));
    ASSERT_TRUE(msgs[0].contains("reasoning_content"));
    EXPECT_EQ(msgs[0]["reasoning_content"].get<std::string>(), "decide to call file_read");
}

} // namespace
