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
#include "session/attachment_store.hpp"
#include "tool/tool_executor.hpp"
#include "utils/utf8_path.hpp"

#include <httplib.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
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
using acecode::ToolCall;
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

// 回归:流式 tool_call 的 name/id 不能被空续传帧覆盖。
//
// 触发场景:部分网关(DeepSeek / wizard-ai code_pilot,PUB-* 模型)在 tool_call 的
// 续传 delta 里发 "name":"" / "id":""(空字符串,而非像标准 OpenAI 那样省略)。
// bug 表现:旧逻辑只判 !is_null,会把首帧捕获到的真实 name/id 覆盖成空 →
// ChatResponse 的 ToolCall.function_name 为空 → agent_loop 报 "Unknown tool"
// 并空转(只在用这类网关的机器上复现,gpt-4o/copilot 续传省略 name 故无碍)。
// 期望:首帧的 name="glob" / id="call_1" 保留,arguments 跨帧正确拼接。
TEST(OpenAiProviderReasoningTest, SseToolCallNameSurvivesEmptyContinuationFrames) {
    LocalHttpServer server([](httplib::Server& s) {
        s.Post("/chat/completions", [](const httplib::Request&, httplib::Response& res) {
            // 首帧:id+name+空 arguments;续传两帧:name/id 为空串,只续 arguments。
            std::string body =
                "data: {\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,\"id\":\"call_1\",\"type\":\"function\",\"function\":{\"name\":\"glob\",\"arguments\":\"\"}}]}}]}\n\n"
                "data: {\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,\"id\":\"\",\"function\":{\"name\":\"\",\"arguments\":\"{\\\"pattern\\\":\\\"*\\\",\"}}]}}]}\n\n"
                "data: {\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,\"id\":\"\",\"function\":{\"name\":\"\",\"arguments\":\"\\\"path\\\":\\\".\\\"}\"}}]}}]}\n\n"
                "data: [DONE]\n\n";
            res.set_content(body, "text/event-stream");
            res.status = 200;
        });
    });

    OpenAiCompatProvider provider(
        "http://127.0.0.1:" + std::to_string(server.port), "", "test-model");
    ChatMessage user_msg;
    user_msg.role = "user";
    user_msg.content = "list files";
    std::vector<ChatMessage> messages = {user_msg};
    std::vector<ToolDef> tools;

    std::vector<ToolCall> tool_calls;
    std::mutex mu;
    auto cb = [&](const StreamEvent& evt) {
        std::lock_guard<std::mutex> lk(mu);
        if (evt.type == StreamEventType::ToolCall) tool_calls.push_back(evt.tool_call);
    };

    std::atomic<bool> abort_flag{false};
    provider.chat_stream(messages, tools, cb, &abort_flag);

    ASSERT_EQ(tool_calls.size(), 1u);
    EXPECT_EQ(tool_calls[0].function_name, "glob");  // 未被空续传帧覆盖(核心断言)
    EXPECT_EQ(tool_calls[0].id, "call_1");
    EXPECT_EQ(tool_calls[0].function_arguments, "{\"pattern\":\"*\",\"path\":\".\"}");
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
    assistant_msg.tool_calls = nlohmann::json::array({
        nlohmann::json{
            {"id", "call_1"},
            {"type", "function"},
            {"function", {{"name", "file_read"}, {"arguments", "{}"}}}
        }
    });
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
    // 注:build_request_body 现在会为 orphan tool_call 合成 stub tool 消息以避免
    // OpenAI 端 400(详见用例 OrphanToolCall*)。本用例输入是单条 assistant 带
    // tool_calls 但没有 tool 结果,所以会被补一条 stub —— 因此 size=2。
    // 我们关心的是 reasoning_content 仍正确回传到 assistant 消息。
    ASSERT_EQ(msgs.size(), 2u);
    ASSERT_TRUE(msgs[0].contains("tool_calls"));
    ASSERT_TRUE(msgs[0].contains("reasoning_content"));
    EXPECT_EQ(msgs[0]["reasoning_content"].get<std::string>(), "decide to call file_read");
    // 第 2 条是合成 stub。
    EXPECT_EQ(msgs[1].value("role", std::string{}), "tool");
    EXPECT_EQ(msgs[1].value("tool_call_id", std::string{}), "call_1");
}

// 用例 7(回归测试,2026-05-08 由用户实际 session 触发):
// **触发场景**:消息序列里中间出现一条 orphan assistant tool_call —— 即
//   [user, assistant{tool_calls=[id=X]}, user] —— assistant 后面紧跟着的不是
//   tool 结果消息而是 user 消息。这种状态由 daemon 中途被 kill / 工具执行抛异常
//   未持久化 / resume 选了截断在 assistant 之后的 jsonl 等场景产生。
// **期望行为**:build_request_body 在 orphan assistant 后自动插入一条
//   role=tool, tool_call_id=X, content=非空占位文本 的 stub 消息,把 4 条总长度
//   保持顺序为 [user, assistant, tool(stub), user]。orphan 的原 user 消息不能
//   被吞、顺序不能错位。
// **修复前 bug 表现**:OpenAI Chat Completions 端会以 HTTP 400
//   `{"error":{"message":"No tool output found for function call …"}}` 拒绝整个
//   请求 —— 该 session 之后任何用户输入都会持续 400,session 永久卡死(用户的
//   `20260506-140715-3c8b` 卡了 2 天就是这个原因)。
TEST(OpenAiProviderReasoningTest, OrphanToolCallGetsPlaceholderToolMessage) {
    TestableProvider provider("http://example.invalid", "", "test-model");

    // 模拟一个被打断的 session:user 提问 → assistant 发起 web_search 调用 →
    // (tool 结果丢失) → user 又发了一条新消息。
    ChatMessage u1;
    u1.role = "user";
    u1.content = "解读下今天百度头条";

    ChatMessage orphan;
    orphan.role = "assistant";
    orphan.content = "";
    orphan.tool_calls = nlohmann::json::array({
        nlohmann::json{
            {"id", "call_VLsQS3a2qyysX8yMojt23dso"},
            {"type", "function"},
            {"function", {
                {"name", "web_search"},
                {"arguments", "{\"limit\":8,\"query\":\"百度热搜\"}"}
            }}
        }
    });

    ChatMessage u2;
    u2.role = "user";
    u2.content = "再解读一下";

    std::vector<ChatMessage> messages = {u1, orphan, u2};
    std::vector<ToolDef> tools;

    auto body = provider.build_request_body(messages, tools, /*stream=*/false);
    const auto& msgs = body["messages"];
    // 期望:user, assistant(tool_calls), tool(stub), user — 共 4 条。
    ASSERT_EQ(msgs.size(), 4u);
    EXPECT_EQ(msgs[0].value("role", std::string{}), "user");
    EXPECT_EQ(msgs[1].value("role", std::string{}), "assistant");
    EXPECT_EQ(msgs[2].value("role", std::string{}), "tool");
    EXPECT_EQ(msgs[2].value("tool_call_id", std::string{}),
              "call_VLsQS3a2qyysX8yMojt23dso");
    // stub content 需要明确传达"被中断",不能是空字符串(某些 endpoint 会拒)。
    EXPECT_FALSE(msgs[2].value("content", std::string{}).empty());
    EXPECT_EQ(msgs[3].value("role", std::string{}), "user");
    EXPECT_EQ(msgs[3]["content"].get<std::string>(), "再解读一下");
}

// 用例 8(防误触保护):
// **触发场景**:正常配对的 [assistant{tool_calls=[id=call_abc]}, tool{call_id=call_abc}]
//   —— 这是健康会话最常见的形态,真实 tool 结果已经存在。
// **期望行为**:build_request_body 输出仍然只有 2 条,顺序不变,真实 tool 内容
//   ("real result")保持不变。**不能**因为补漏逻辑误塞额外的 stub。
// **为什么写这个用例**:用例 7 的 orphan 检测如果实现成"任何 assistant.tool_calls
//   都补一条",就会让健康会话的每个工具调用后多出一条空 stub —— context 翻倍 +
//   消息顺序错乱。这条用例守住"只补漏不重复"的边界。
TEST(OpenAiProviderReasoningTest, MatchedToolCallStaysIntactNoExtraStub) {
    TestableProvider provider("http://example.invalid", "", "test-model");

    ChatMessage assistant_msg;
    assistant_msg.role = "assistant";
    assistant_msg.content = "";
    assistant_msg.tool_calls = nlohmann::json::array({
        nlohmann::json{
            {"id", "call_abc"},
            {"type", "function"},
            {"function", {
                {"name", "file_read"},
                {"arguments", "{\"file_path\":\"a.md\"}"}
            }}
        }
    });

    ChatMessage tool_msg;
    tool_msg.role = "tool";
    tool_msg.content = "real result";
    tool_msg.tool_call_id = "call_abc";

    std::vector<ChatMessage> messages = {assistant_msg, tool_msg};
    std::vector<ToolDef> tools;

    auto body = provider.build_request_body(messages, tools, /*stream=*/false);
    const auto& msgs = body["messages"];
    ASSERT_EQ(msgs.size(), 2u);
    EXPECT_EQ(msgs[0].value("role", std::string{}), "assistant");
    EXPECT_EQ(msgs[1].value("role", std::string{}), "tool");
    EXPECT_EQ(msgs[1]["content"].get<std::string>(), "real result");
}

// 用例 8.1(回归测试,2026-06-08 用户真实 session 触发):
// 历史 assistant.tool_calls 中 memory_read 的 function.arguments 被持久化为
// `{}""`。多个 OpenAI-compatible 网关都会在恢复后的下一次请求里 400:
// "unexpected content after document" / "function.arguments ... must be JSON"。
// 请求出口应把这类"完整 JSON 后面多出尾巴"的历史参数修回合法 JSON。
TEST(OpenAiProviderReasoningTest, CorruptPersistedToolArgumentsAreTrimmed) {
    TestableProvider provider("http://example.invalid", "", "test-model");

    ChatMessage assistant_msg;
    assistant_msg.role = "assistant";
    assistant_msg.content = "";
    assistant_msg.tool_calls = nlohmann::json::array({
        nlohmann::json{
            {"id", "call_257a67d0f1c64848a236fddb"},
            {"type", "function"},
            {"function", {
                {"name", "memory_read"},
                {"arguments", "{}\"\""}
            }}
        }
    });

    ChatMessage tool_msg;
    tool_msg.role = "tool";
    tool_msg.content = "memory result";
    tool_msg.tool_call_id = "call_257a67d0f1c64848a236fddb";

    auto body = provider.build_request_body({assistant_msg, tool_msg}, {}, false);
    const auto& msgs = body["messages"];
    ASSERT_EQ(msgs.size(), 2u);
    ASSERT_TRUE(msgs[0].contains("tool_calls"));
    const auto& tool_calls = msgs[0]["tool_calls"];
    ASSERT_EQ(tool_calls.size(), 1u);
    EXPECT_EQ(tool_calls[0]["function"]["arguments"].get<std::string>(), "{}");
}

// 用例 8.2:健康历史不应被规范化逻辑重写。这里保留空格顺序,证明合法
// arguments 字符串按原样发给 provider。
TEST(OpenAiProviderReasoningTest, ValidPersistedToolArgumentsStayUnchanged) {
    TestableProvider provider("http://example.invalid", "", "test-model");

    const std::string raw_args = "{\"path\": \"D:/1000src/mailsdk\", \"pattern\": \"**/*.md\"}";
    ChatMessage assistant_msg;
    assistant_msg.role = "assistant";
    assistant_msg.content = "";
    assistant_msg.tool_calls = nlohmann::json::array({
        nlohmann::json{
            {"id", "call_valid"},
            {"type", "function"},
            {"function", {
                {"name", "glob"},
                {"arguments", raw_args}
            }}
        }
    });

    ChatMessage tool_msg;
    tool_msg.role = "tool";
    tool_msg.content = "glob result";
    tool_msg.tool_call_id = "call_valid";

    auto body = provider.build_request_body({assistant_msg, tool_msg}, {}, false);
    const auto& args =
        body["messages"][0]["tool_calls"][0]["function"]["arguments"];
    EXPECT_EQ(args.get<std::string>(), raw_args);
}

TEST(OpenAiProviderReasoningTest, LegacySingleToolCallObjectIsWrappedForRequest) {
    TestableProvider provider("http://example.invalid", "", "test-model");

    ChatMessage assistant_msg;
    assistant_msg.role = "assistant";
    assistant_msg.tool_calls = nlohmann::json{
        {"id", "call_legacy"},
        {"type", "function"},
        {"function", {{"name", "file_read"}, {"arguments", "{}"}}}
    };

    ChatMessage tool_msg;
    tool_msg.role = "tool";
    tool_msg.content = "file contents";
    tool_msg.tool_call_id = "call_legacy";

    auto body = provider.build_request_body({assistant_msg, tool_msg}, {}, false);
    const auto& msgs = body["messages"];
    ASSERT_EQ(msgs.size(), 2u);
    ASSERT_TRUE(msgs[0].contains("tool_calls"));
    ASSERT_TRUE(msgs[0]["tool_calls"].is_array());
    ASSERT_EQ(msgs[0]["tool_calls"].size(), 1u);
    EXPECT_EQ(msgs[0]["tool_calls"][0]["id"].get<std::string>(), "call_legacy");
    EXPECT_EQ(msgs[1].value("tool_call_id", std::string{}), "call_legacy");
}

// 用例:ToolExecutor 提供确定性工具顺序,OpenAI-compatible request body
// 通过 tools array 发送 schema,不依赖 system prompt 里的重复 schema 文本。
TEST(OpenAiProviderReasoningTest, BuildRequestBodyUsesDeterministicToolsArray) {
    TestableProvider provider("http://example.invalid", "", "test-model");

    acecode::ToolExecutor executor;
    auto register_tool = [&](std::string name, std::string description) {
        ToolDef def;
        def.name = std::move(name);
        def.description = std::move(description);
        def.parameters = {
            {"type", "object"},
            {"properties", {
                {"value", {{"type", "string"}}}
            }},
        };
        acecode::ToolImpl impl;
        impl.definition = def;
        impl.execute = [](const std::string&, const acecode::ToolContext&) {
            return acecode::ToolResult{"ok", true};
        };
        executor.register_tool(impl);
    };

    register_tool("z_tool", "Z tool");
    register_tool("a_tool", "A tool");

    ChatMessage user;
    user.role = "user";
    user.content = "hello";
    auto body = provider.build_request_body({user}, executor.get_tool_definitions(), false);

    ASSERT_TRUE(body.contains("tools"));
    const auto& tools = body["tools"];
    ASSERT_EQ(tools.size(), 2u);
    EXPECT_EQ(tools[0]["function"]["name"].get<std::string>(), "a_tool");
    EXPECT_EQ(tools[1]["function"]["name"].get<std::string>(), "z_tool");
    EXPECT_EQ(tools[0]["function"]["parameters"]["properties"]["value"]["type"].get<std::string>(),
              "string");
}

// 用例 9(并行工具调用的部分 orphan):
// **触发场景**:assistant 一回合并行发起两个 tool_calls(id=call_a, call_b),
//   持久化后只有 call_a 的 tool 结果落了盘,call_b 的中途丢失(工具抛异常 /
//   并发执行被 abort)。消息序列变成 [assistant{tool_calls=[a,b]}, tool{a}]。
// **期望行为**:输出 3 条 [assistant, tool{a, real result}, tool{b, stub}]。
//   call_a 的真实结果原样保留(content="grep result"),call_b 的 stub 紧跟在后,
//   顺序不能颠倒(OpenAI 不要求 id 顺序,但 tool 消息必须连续夹在 assistant 和
//   下一个非 tool 消息之间)。
// **修复前 bug 表现**:同用例 7,OpenAI 会 400 提示 `call_b` 没有 tool output;
//   即便 call_a 是好的,整个请求仍被拒。
TEST(OpenAiProviderReasoningTest, PartialOrphanInParallelToolCalls) {
    TestableProvider provider("http://example.invalid", "", "test-model");

    ChatMessage assistant_msg;
    assistant_msg.role = "assistant";
    assistant_msg.content = "";
    assistant_msg.tool_calls = nlohmann::json::array({
        nlohmann::json{
            {"id", "call_a"},
            {"type", "function"},
            {"function", {{"name", "grep"}, {"arguments", "{}"}}}
        },
        nlohmann::json{
            {"id", "call_b"},
            {"type", "function"},
            {"function", {{"name", "glob"}, {"arguments", "{}"}}}
        }
    });

    // 只有 call_a 有结果,call_b 是 orphan。
    ChatMessage tool_a;
    tool_a.role = "tool";
    tool_a.content = "grep result";
    tool_a.tool_call_id = "call_a";

    std::vector<ChatMessage> messages = {assistant_msg, tool_a};
    std::vector<ToolDef> tools;

    auto body = provider.build_request_body(messages, tools, /*stream=*/false);
    const auto& msgs = body["messages"];
    // 期望:assistant, tool(call_a, 真实), tool(call_b, stub) — 共 3 条。
    ASSERT_EQ(msgs.size(), 3u);
    EXPECT_EQ(msgs[0].value("role", std::string{}), "assistant");
    EXPECT_EQ(msgs[1].value("tool_call_id", std::string{}), "call_a");
    EXPECT_EQ(msgs[1]["content"].get<std::string>(), "grep result");
    EXPECT_EQ(msgs[2].value("tool_call_id", std::string{}), "call_b");
    // call_b 的 content 应该是 stub(非空)。
    EXPECT_FALSE(msgs[2].value("content", std::string{}).empty());
}

// 用例 10:历史里存在游离的 role=tool 消息。
//
// 这类消息可能来自旧版本伪角色持久化、手工拼接 JSONL、或截断恢复。OpenAI
// Chat Completions 要求 tool 消息必须紧跟在带同 id tool_calls 的 assistant
// 后面；游离 tool 若原样发出会让后续每次 user 继续都 400。
TEST(OpenAiProviderReasoningTest, StandaloneToolMessageIsDropped) {
    TestableProvider provider("http://example.invalid", "", "test-model");

    ChatMessage u1;
    u1.role = "user";
    u1.content = "first";

    ChatMessage tool_msg;
    tool_msg.role = "tool";
    tool_msg.content = "stale result";
    tool_msg.tool_call_id = "call_stale";

    ChatMessage u2;
    u2.role = "user";
    u2.content = "continue";

    auto body = provider.build_request_body({u1, tool_msg, u2}, {}, false);
    const auto& msgs = body["messages"];
    ASSERT_EQ(msgs.size(), 2u);
    EXPECT_EQ(msgs[0].value("role", std::string{}), "user");
    EXPECT_EQ(msgs[0]["content"].get<std::string>(), "first");
    EXPECT_EQ(msgs[1].value("role", std::string{}), "user");
    EXPECT_EQ(msgs[1]["content"].get<std::string>(), "continue");
}

// 用例 11:assistant 后的 tool block 含有未被请求的额外 tool_call_id。
//
// 修复前出口会把连续 tool 消息全部保留,即使 id 不在前一条 assistant.tool_calls
// 中。严格后端会把这条 extra tool 当成非法历史并返回 400。
TEST(OpenAiProviderReasoningTest, UnexpectedToolResultAfterAssistantIsDropped) {
    TestableProvider provider("http://example.invalid", "", "test-model");

    ChatMessage assistant_msg;
    assistant_msg.role = "assistant";
    assistant_msg.tool_calls = nlohmann::json::array({
        nlohmann::json{
            {"id", "call_expected"},
            {"type", "function"},
            {"function", {{"name", "grep"}, {"arguments", "{}"}}}
        }
    });

    ChatMessage expected_tool;
    expected_tool.role = "tool";
    expected_tool.content = "expected result";
    expected_tool.tool_call_id = "call_expected";

    ChatMessage extra_tool;
    extra_tool.role = "tool";
    extra_tool.content = "extra result";
    extra_tool.tool_call_id = "call_extra";

    auto body = provider.build_request_body(
        {assistant_msg, expected_tool, extra_tool}, {}, false);
    const auto& msgs = body["messages"];
    ASSERT_EQ(msgs.size(), 2u);
    EXPECT_EQ(msgs[0].value("role", std::string{}), "assistant");
    EXPECT_EQ(msgs[1].value("role", std::string{}), "tool");
    EXPECT_EQ(msgs[1].value("tool_call_id", std::string{}), "call_expected");
    EXPECT_EQ(msgs[1]["content"].get<std::string>(), "expected result");
}

// 用例 12:同一个 tool_call_id 被重复写入两条 tool 结果。
//
// 请求出口保留第一条真实结果并丢弃后续重复项,避免一条 tool_call 被回答多次。
TEST(OpenAiProviderReasoningTest, DuplicateToolResultKeepsFirstResult) {
    TestableProvider provider("http://example.invalid", "", "test-model");

    ChatMessage assistant_msg;
    assistant_msg.role = "assistant";
    assistant_msg.tool_calls = nlohmann::json::array({
        nlohmann::json{
            {"id", "call_dupe"},
            {"type", "function"},
            {"function", {{"name", "file_read"}, {"arguments", "{}"}}}
        }
    });

    ChatMessage first_tool;
    first_tool.role = "tool";
    first_tool.content = "first result";
    first_tool.tool_call_id = "call_dupe";

    ChatMessage second_tool;
    second_tool.role = "tool";
    second_tool.content = "second result";
    second_tool.tool_call_id = "call_dupe";

    auto body = provider.build_request_body(
        {assistant_msg, first_tool, second_tool}, {}, false);
    const auto& msgs = body["messages"];
    ASSERT_EQ(msgs.size(), 2u);
    EXPECT_EQ(msgs[1].value("tool_call_id", std::string{}), "call_dupe");
    EXPECT_EQ(msgs[1]["content"].get<std::string>(), "first result");
}

// 用例 13:assistant.tool_calls 数组里混入无 id 的坏条目。
//
// 坏条目无法在 Chat Completions 协议中被 tool 结果引用,必须在请求出口丢弃；
// 同一数组里的健康 tool_call 仍应保留并正常匹配 tool result。
TEST(OpenAiProviderReasoningTest, MalformedToolCallWithoutIdIsDropped) {
    TestableProvider provider("http://example.invalid", "", "test-model");

    ChatMessage assistant_msg;
    assistant_msg.role = "assistant";
    assistant_msg.tool_calls = nlohmann::json::array({
        nlohmann::json{
            {"type", "function"},
            {"function", {{"name", "grep"}, {"arguments", "{}"}}}
        },
        nlohmann::json{
            {"id", "call_valid"},
            {"type", "function"},
            {"function", {{"name", "glob"}, {"arguments", "{}"}}}
        }
    });

    ChatMessage tool_msg;
    tool_msg.role = "tool";
    tool_msg.content = "valid result";
    tool_msg.tool_call_id = "call_valid";

    auto body = provider.build_request_body({assistant_msg, tool_msg}, {}, false);
    const auto& msgs = body["messages"];
    ASSERT_EQ(msgs.size(), 2u);
    ASSERT_TRUE(msgs[0].contains("tool_calls"));
    const auto& tool_calls = msgs[0]["tool_calls"];
    ASSERT_EQ(tool_calls.size(), 1u);
    EXPECT_EQ(tool_calls[0]["id"].get<std::string>(), "call_valid");
    EXPECT_EQ(msgs[1].value("tool_call_id", std::string{}), "call_valid");
    EXPECT_EQ(msgs[1]["content"].get<std::string>(), "valid result");
}

// 用例 14:assistant.tool_calls 数组里重复声明同一个 id。
//
// 重复 id 无法和 tool 结果形成一一对应关系；请求出口保留第一条声明并丢弃后续
// 重复项，避免 provider 看到同一 tool_call_id 被声明多次。
TEST(OpenAiProviderReasoningTest, DuplicateAssistantToolCallIdIsDropped) {
    TestableProvider provider("http://example.invalid", "", "test-model");

    ChatMessage assistant_msg;
    assistant_msg.role = "assistant";
    assistant_msg.tool_calls = nlohmann::json::array({
        nlohmann::json{
            {"id", "call_same"},
            {"type", "function"},
            {"function", {{"name", "grep"}, {"arguments", "{}"}}}
        },
        nlohmann::json{
            {"id", "call_same"},
            {"type", "function"},
            {"function", {{"name", "glob"}, {"arguments", "{}"}}}
        }
    });

    ChatMessage tool_msg;
    tool_msg.role = "tool";
    tool_msg.content = "first call result";
    tool_msg.tool_call_id = "call_same";

    auto body = provider.build_request_body({assistant_msg, tool_msg}, {}, false);
    const auto& msgs = body["messages"];
    ASSERT_EQ(msgs.size(), 2u);
    ASSERT_TRUE(msgs[0].contains("tool_calls"));
    const auto& tool_calls = msgs[0]["tool_calls"];
    ASSERT_EQ(tool_calls.size(), 1u);
    EXPECT_EQ(tool_calls[0]["id"].get<std::string>(), "call_same");
    EXPECT_EQ(tool_calls[0]["function"]["name"].get<std::string>(), "grep");
    EXPECT_EQ(msgs[1]["content"].get<std::string>(), "first call result");
}

TEST(OpenAiProviderReasoningTest, SystemMessagesAreCoalescedAtRequestStart) {
    TestableProvider provider("http://example.invalid", "", "test-model");

    ChatMessage static_system;
    static_system.role = "system";
    static_system.content = "static instructions";

    ChatMessage session_context;
    session_context.role = "user";
    session_context.content = "session context";

    ChatMessage compact_summary;
    compact_summary.role = "system";
    compact_summary.content = "compact summary";
    compact_summary.is_compact_summary = true;

    ChatMessage user_msg;
    user_msg.role = "user";
    user_msg.content = "continue";

    auto body = provider.build_request_body(
        {static_system, session_context, compact_summary, user_msg}, {}, false);
    const auto& msgs = body["messages"];

    ASSERT_EQ(msgs.size(), 3u);
    ASSERT_EQ(msgs[0]["role"].get<std::string>(), "system");
    const auto system_content = msgs[0]["content"].get<std::string>();
    EXPECT_NE(system_content.find("static instructions"), std::string::npos);
    EXPECT_NE(system_content.find("compact summary"), std::string::npos);
    EXPECT_EQ(msgs[1]["role"].get<std::string>(), "user");
    EXPECT_EQ(msgs[1]["content"].get<std::string>(), "session context");
    EXPECT_EQ(msgs[2]["role"].get<std::string>(), "user");
    EXPECT_EQ(msgs[2]["content"].get<std::string>(), "continue");
}

TEST(OpenAiProviderReasoningTest, SystemMessageBetweenAssistantAndToolKeepsToolResult) {
    TestableProvider provider("http://example.invalid", "", "test-model");

    ChatMessage assistant_msg;
    assistant_msg.role = "assistant";
    assistant_msg.tool_calls = nlohmann::json::array({
        nlohmann::json{
            {"id", "call_1"},
            {"type", "function"},
            {"function", {{"name", "file_read"}, {"arguments", "{}"}}}
        }
    });

    ChatMessage injected_system;
    injected_system.role = "system";
    injected_system.content = "late system note";

    ChatMessage tool_msg;
    tool_msg.role = "tool";
    tool_msg.content = "real output";
    tool_msg.tool_call_id = "call_1";

    auto body = provider.build_request_body(
        {assistant_msg, injected_system, tool_msg}, {}, false);
    const auto& msgs = body["messages"];

    ASSERT_EQ(msgs.size(), 3u);
    EXPECT_EQ(msgs[0]["role"].get<std::string>(), "system");
    EXPECT_EQ(msgs[1]["role"].get<std::string>(), "assistant");
    EXPECT_EQ(msgs[2]["role"].get<std::string>(), "tool");
    EXPECT_EQ(msgs[2]["tool_call_id"].get<std::string>(), "call_1");
    EXPECT_EQ(msgs[2]["content"].get<std::string>(), "real output");
}

TEST(OpenAiProviderReasoningTest, ImageContentPartSerializesAsDataUrl) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path() / "acecode_openai_image_part_test";
    fs::remove_all(dir);
    fs::create_directories(dir);
    auto image_path = dir / "screen.png";
    {
        std::ofstream ofs(image_path, std::ios::binary);
        ofs << "abc";
    }

    acecode::AttachmentRecord record;
    record.id = "att_test";
    record.session_id = "session1";
    record.name = "screen.png";
    record.kind = "image";
    record.mime_type = "image/png";
    record.path = acecode::path_to_utf8(image_path);
    record.blob_url = "/api/sessions/session1/attachments/att_test/blob";
    record.size_bytes = 3;

    ChatMessage user;
    user.role = "user";
    user.content = "what is in this image?";
    user.content_parts = nlohmann::json::array({
        {{"type", "text"}, {"text", user.content}},
        {{"type", "image"}, {"attachment", acecode::attachment_to_json(record)}},
    });

    TestableProvider provider("http://example.invalid", "", "test-model");
    auto body = provider.build_request_body({user}, {}, false);
    const auto& content = body["messages"][0]["content"];

    ASSERT_TRUE(content.is_array());
    ASSERT_EQ(content.size(), 2u);
    EXPECT_EQ(content[0]["type"], "text");
    EXPECT_EQ(content[1]["type"], "image_url");
    EXPECT_EQ(content[1]["image_url"]["url"], "data:image/png;base64,YWJj");

    fs::remove_all(dir);
}

TEST(OpenAiProviderReasoningTest, TextOnlyUserMessageKeepsStringContent) {
    ChatMessage user;
    user.role = "user";
    user.content = "plain text only";

    TestableProvider provider("http://example.invalid", "", "test-model");
    auto body = provider.build_request_body({user}, {}, false);
    const auto& content = body["messages"][0]["content"];

    ASSERT_TRUE(content.is_string());
    EXPECT_EQ(content.get<std::string>(), "plain text only");
}

TEST(OpenAiProviderReasoningTest, FileContentPartSerializesAsTextContext) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path() / "acecode_openai_file_part_test";
    fs::remove_all(dir);
    fs::create_directories(dir);
    auto file_path = dir / "notes.txt";
    {
        std::ofstream ofs(file_path, std::ios::binary);
        ofs << "hello from attached file";
    }

    acecode::AttachmentRecord record;
    record.id = "att_file";
    record.session_id = "session1";
    record.name = "notes.txt";
    record.kind = "file";
    record.mime_type = "text/plain";
    record.path = acecode::path_to_utf8(file_path);
    record.blob_url = "/api/sessions/session1/attachments/att_file/blob";
    record.size_bytes = 24;

    ChatMessage user;
    user.role = "user";
    user.content = "summarize";
    user.content_parts = nlohmann::json::array({
        {{"type", "text"}, {"text", user.content}},
        {{"type", "file"}, {"attachment", acecode::attachment_to_json(record)}},
    });

    TestableProvider provider("http://example.invalid", "", "test-model");
    auto body = provider.build_request_body({user}, {}, false);
    const auto& content = body["messages"][0]["content"];

    ASSERT_TRUE(content.is_array());
    ASSERT_EQ(content.size(), 2u);
    EXPECT_EQ(content[1]["type"], "text");
    const auto text = content[1]["text"].get<std::string>();
    EXPECT_NE(text.find("[Attached file]"), std::string::npos);
    EXPECT_NE(text.find("hello from attached file"), std::string::npos);

    fs::remove_all(dir);
}

TEST(OpenAiProviderReasoningTest, MissingImageContentPartSerializesFallbackText) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path() / "acecode_openai_missing_image_part_test";
    fs::remove_all(dir);
    fs::create_directories(dir);

    acecode::AttachmentRecord record;
    record.id = "att_missing";
    record.session_id = "session1";
    record.name = "missing.png";
    record.kind = "image";
    record.mime_type = "image/png";
    record.path = acecode::path_to_utf8(dir / "missing.png");
    record.blob_url = "/api/sessions/session1/attachments/att_missing/blob";
    record.size_bytes = 10;

    ChatMessage user;
    user.role = "user";
    user.content_parts = nlohmann::json::array({
        {{"type", "image"}, {"attachment", acecode::attachment_to_json(record)}},
    });

    TestableProvider provider("http://example.invalid", "", "test-model");
    auto body = provider.build_request_body({user}, {}, false);
    const auto& content = body["messages"][0]["content"];

    ASSERT_TRUE(content.is_array());
    ASSERT_EQ(content.size(), 1u);
    EXPECT_EQ(content[0]["type"], "text");
    const auto text = content[0]["text"].get<std::string>();
    EXPECT_NE(text.find("Attached image unavailable"), std::string::npos);

    fs::remove_all(dir);
}

} // namespace
