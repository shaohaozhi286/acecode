// 覆盖 src/provider/openai_provider.cpp 中 tool_call 全量快照重发的兼容性解析。
//
// 背景:OpenAI 流式协议规定 tool_calls delta 是增量帧 —— 首帧带 id + function.name
//   + arguments 起始片段,后续帧只带 arguments 的增量片段(通常无 id),客户端
//   做字符串拼接还原完整的 arguments JSON。但部分自建网关 / 非标兼容服务并不
//   遵守这个约定:实测某些公司网关上的 GLM-5.1-FP8 在每一帧 SSE 里都
//   重发 *相同的 id* 和 *完整的* arguments JSON 全量快照。早期实现一律 append
//   会把 GLM 流拼成 `{"command":...}{"command":...}` 两份 JSON 头尾相接,
//   nlohmann::json::parse 必失败,UI 显示 "Failed to parse tool arguments"。
//
// 修复策略(openai_provider.cpp 内):同一 tool_call index 上,若新 chunk 带 id
//   且与已记录的 acc.id 相同,则视为全量帧,arguments 用替换语义而非追加。
//   OpenAI 标准首帧后续不再重复 id,自然走原 append 路径,不破坏标准行为。
//
// 本文件覆盖的场景:
//   1. GLM 风格全量快照重发:每一帧带相同 id + 完整 arguments,期望最终
//      tool_call.function_arguments 是 *单份* 完整 JSON 而非两份拼接。
//   2. 标准 OpenAI 风格增量:首帧带 id + 第一片 arguments,后续帧只带
//      arguments 增量(无 id),期望按字符串拼接还原(回归保护)。
//   3. 多个 tool_call 并发(不同 index)各自独立累积/替换,互不污染。

#include <gtest/gtest.h>

#include "provider/openai_provider.hpp"
#include "provider/llm_provider.hpp"

#include <httplib.h>
#include <nlohmann/json.hpp>

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
using acecode::StreamEvent;
using acecode::StreamEventType;
using acecode::ToolCall;
using acecode::ToolDef;

// 启动本地 httplib server,析构时自动 stop。与同目录其他 provider 测试同形。
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

// 把 chat_stream 跑出的 ToolCall 事件收集成 vector 的便捷工具。
struct StreamCollector {
    std::vector<ToolCall> tool_calls;
    int done_events = 0;
    int error_events = 0;
    std::mutex mu;

    auto callback() {
        return [this](const StreamEvent& evt) {
            std::lock_guard<std::mutex> lk(mu);
            switch (evt.type) {
                case StreamEventType::ToolCall: tool_calls.push_back(evt.tool_call); break;
                case StreamEventType::Done:     ++done_events; break;
                case StreamEventType::Error:    ++error_events; break;
                default: break;
            }
        };
    }
};

// 用例 1:GLM-5.1-FP8 风格 —— 每一帧重发相同 id 和完整 arguments JSON。
// 修复前两帧会被 append 成 `{...}{...}`,parse fail。修复后应只取最后一份快照。
TEST(OpenAiProviderToolCallSnapshotTest, GlmStyleFullSnapshotResendDoesNotDuplicate) {
    // 这是抓包里实际看到的 arguments 内容(已 unescape 一层 SSE 字符串包装)。
    const std::string full_args =
        R"({"command": "dir /b \"D:\\Users\\shaohaozhi661\\testglm\""})";

    LocalHttpServer server([&](httplib::Server& s) {
        s.Post("/chat/completions", [&](const httplib::Request&, httplib::Response& res) {
            // 构造完全模拟 GLM 抓包的 chunk 序列:多个空 content 帧 →
            // 两条相同的 tool_call 全量帧 → usage 帧 → [DONE]。
            auto make_tool_chunk = [&]() {
                nlohmann::json j;
                j["choices"] = nlohmann::json::array();
                nlohmann::json choice;
                choice["index"] = 0;
                choice["delta"]["content"] = "";
                choice["delta"]["tool_calls"] = nlohmann::json::array();
                nlohmann::json tc;
                tc["id"] = "call_143a70544e0b4db5bb70b316";
                tc["type"] = "function";
                tc["index"] = 0;
                tc["function"]["name"] = "bash";
                tc["function"]["arguments"] = full_args;
                choice["delta"]["tool_calls"].push_back(tc);
                j["choices"].push_back(choice);
                return "data: " + j.dump() + "\n\n";
            };
            std::string body;
            body += "data: {\"choices\":[{\"delta\":{\"content\":\"\"},\"index\":0}]}\n\n";
            body += "data: {\"choices\":[{\"delta\":{\"content\":\"\"},\"index\":0}]}\n\n";
            body += make_tool_chunk();
            body += make_tool_chunk();
            // 最终帧:finish_reason=tool_calls,触发 flush。
            body += "data: {\"choices\":[{\"delta\":{},\"index\":0,\"finish_reason\":\"tool_calls\"}]}\n\n";
            body += "data: {\"usage\":{\"prompt_tokens\":0,\"completion_tokens\":0,\"total_tokens\":0},\"choices\":[]}\n\n";
            body += "data: [DONE]\n\n";
            res.set_content(body, "text/event-stream");
            res.status = 200;
        });
    });

    OpenAiCompatProvider provider(
        "http://127.0.0.1:" + std::to_string(server.port), "", "GLM-5.1-FP8");

    ChatMessage user_msg;
    user_msg.role = "user";
    user_msg.content = "list testglm dir";
    std::vector<ChatMessage> messages = {user_msg};
    std::vector<ToolDef> tools;

    StreamCollector col;
    std::atomic<bool> abort_flag{false};
    provider.chat_stream(messages, tools, col.callback(), &abort_flag);

    ASSERT_EQ(col.tool_calls.size(), 1u) << "全量重发不应放大 tool_call 数量";
    EXPECT_EQ(col.tool_calls[0].id, "call_143a70544e0b4db5bb70b316");
    EXPECT_EQ(col.tool_calls[0].function_name, "bash");
    // 关键断言:arguments 必须是 *单份* 完整 JSON,不能被拼接成两份。
    EXPECT_EQ(col.tool_calls[0].function_arguments, full_args);

    // arguments 必须能被 nlohmann::json::parse 成功消化(这正是修复前 fail 的点)。
    auto parsed = nlohmann::json::parse(col.tool_calls[0].function_arguments, nullptr, false);
    ASSERT_FALSE(parsed.is_discarded()) << "arguments 必须是合法 JSON";
    EXPECT_EQ(parsed.value("command", ""),
              "dir /b \"D:\\Users\\shaohaozhi661\\testglm\"");

    EXPECT_EQ(col.done_events, 1);
    EXPECT_EQ(col.error_events, 0);
}

// 用例 2:OpenAI 标准增量回归保护 —— 首帧带 id + name + 第一片 arguments,
// 后续帧无 id 仅带 arguments 增量片段,客户端必须按字符串拼接还原。
// 验证修复没有破坏标准协议。
TEST(OpenAiProviderToolCallSnapshotTest, StandardOpenAiIncrementalDeltaStillConcatenates) {
    LocalHttpServer server([](httplib::Server& s) {
        s.Post("/chat/completions", [](const httplib::Request&, httplib::Response& res) {
            // 标准 OpenAI:
            //   chunk 1: id + name + arguments=`{"loca`
            //   chunk 2: 仅 arguments=`tion": "SF"}`
            std::string body;
            body += "data: {\"choices\":[{\"delta\":{\"tool_calls\":[{"
                    "\"id\":\"call_abc\",\"type\":\"function\",\"index\":0,"
                    "\"function\":{\"name\":\"get_weather\",\"arguments\":\"{\\\"loca\"}}]},\"index\":0}]}\n\n";
            body += "data: {\"choices\":[{\"delta\":{\"tool_calls\":[{"
                    "\"index\":0,\"function\":{\"arguments\":\"tion\\\": \\\"SF\\\"}\"}}]},\"index\":0}]}\n\n";
            body += "data: {\"choices\":[{\"delta\":{},\"index\":0,\"finish_reason\":\"tool_calls\"}]}\n\n";
            body += "data: [DONE]\n\n";
            res.set_content(body, "text/event-stream");
            res.status = 200;
        });
    });

    OpenAiCompatProvider provider(
        "http://127.0.0.1:" + std::to_string(server.port), "", "test-model");

    ChatMessage user_msg;
    user_msg.role = "user";
    user_msg.content = "weather";
    std::vector<ChatMessage> messages = {user_msg};
    std::vector<ToolDef> tools;

    StreamCollector col;
    std::atomic<bool> abort_flag{false};
    provider.chat_stream(messages, tools, col.callback(), &abort_flag);

    ASSERT_EQ(col.tool_calls.size(), 1u);
    EXPECT_EQ(col.tool_calls[0].id, "call_abc");
    EXPECT_EQ(col.tool_calls[0].function_name, "get_weather");
    // 关键:增量必须按拼接还原,而不是被错误地"替换"成最后一帧的尾段。
    EXPECT_EQ(col.tool_calls[0].function_arguments, R"({"location": "SF"})");
    EXPECT_EQ(col.error_events, 0);
}

// 用例 X1:`monotoo.shop` 风格 —— 完整 tool_call 帧之后跟一个 *相同 id +
// arguments=""* 的"确认帧"。早期实现把所有同 id 的后续帧都当作全量快照重发,
// 用空字符串覆盖了已累积的完整参数,导致 ToolExecutor 拿到空 args:
//   `Web search failed: invalid JSON arguments: [json.exception.parse_error.101]
//    parse error at line 1, column 1: attempting to parse an empty input`
// 修复:incoming_args 为空时,无论是否被判为全量重发,都跳过(不覆盖、不追加),
// 已累积的 arguments 保持原样;finish_reason flush 时仍能拿到完整 JSON。
TEST(OpenAiProviderToolCallSnapshotTest, EmptyArgsConfirmationFrameDoesNotEraseAccumulated) {
    const std::string full_args = R"({"query":"latest news today","limit":5})";

    LocalHttpServer server([&](httplib::Server& s) {
        s.Post("/chat/completions", [&](const httplib::Request&, httplib::Response& res) {
            std::string body;
            // 第 1 帧:完整 tool_call(id + name + 完整 arguments JSON)。
            {
                nlohmann::json j;
                nlohmann::json choice;
                choice["index"] = 0;
                choice["delta"]["tool_calls"] = nlohmann::json::array();
                nlohmann::json tc;
                tc["id"] = "call_zzz";
                tc["type"] = "function";
                tc["index"] = 0;
                tc["function"]["name"] = "web_search";
                tc["function"]["arguments"] = full_args;
                choice["delta"]["tool_calls"].push_back(tc);
                j["choices"].push_back(choice);
                body += "data: " + j.dump() + "\n\n";
            }
            // 第 2 帧:同 id,但 arguments="" —— 实测 monotoo.shop 网关在工具调用
            // 完成后会发一个这样的"心跳/确认帧"。修复前会用 "" 把第 1 帧累积的
            // 完整 JSON 直接抹平。
            {
                nlohmann::json j;
                nlohmann::json choice;
                choice["index"] = 0;
                choice["delta"]["tool_calls"] = nlohmann::json::array();
                nlohmann::json tc;
                tc["id"] = "call_zzz";
                tc["index"] = 0;
                tc["function"]["arguments"] = "";
                choice["delta"]["tool_calls"].push_back(tc);
                j["choices"].push_back(choice);
                body += "data: " + j.dump() + "\n\n";
            }
            body += "data: {\"choices\":[{\"delta\":{},\"index\":0,\"finish_reason\":\"tool_calls\"}]}\n\n";
            body += "data: [DONE]\n\n";
            res.set_content(body, "text/event-stream");
            res.status = 200;
        });
    });

    OpenAiCompatProvider provider(
        "http://127.0.0.1:" + std::to_string(server.port), "", "test-model");

    ChatMessage user_msg;
    user_msg.role = "user";
    user_msg.content = "search news";
    std::vector<ChatMessage> messages = {user_msg};
    std::vector<ToolDef> tools;

    StreamCollector col;
    std::atomic<bool> abort_flag{false};
    provider.chat_stream(messages, tools, col.callback(), &abort_flag);

    ASSERT_EQ(col.tool_calls.size(), 1u);
    EXPECT_EQ(col.tool_calls[0].id, "call_zzz");
    EXPECT_EQ(col.tool_calls[0].function_name, "web_search");
    // 关键:空确认帧不能擦除已累积的完整 arguments。
    EXPECT_EQ(col.tool_calls[0].function_arguments, full_args);
    auto parsed = nlohmann::json::parse(col.tool_calls[0].function_arguments,
                                         nullptr, false);
    ASSERT_FALSE(parsed.is_discarded());
    EXPECT_EQ(parsed.value("query", ""), "latest news today");
    EXPECT_EQ(parsed.value("limit", 0), 5);
    EXPECT_EQ(col.error_events, 0);
}

// 用例 X2:伪快照 —— 网关每一帧都重发相同 id,但 arguments 是 *增量片段*
// 而非全量快照(既不是 OpenAI 标准的"后续帧无 id",也不是 GLM 的"全量快照")。
// 此时若一律按 id 相同视为快照替换,只剩最后一帧的尾段,JSON 残缺。
// 修复:仅当 incoming_args 包含 acc.arguments 作为前缀(真正的快照增长)
// 才用替换;否则按 delta 追加,还原完整 JSON。
TEST(OpenAiProviderToolCallSnapshotTest, RepeatedIdWithDeltaArgumentsAppendsInsteadOfReplacing) {
    LocalHttpServer server([](httplib::Server& s) {
        s.Post("/chat/completions", [](const httplib::Request&, httplib::Response& res) {
            std::string body;
            // 第 1 帧:id + name + arguments 起始片段 `{"command":"l`
            body += "data: {\"choices\":[{\"delta\":{\"tool_calls\":[{"
                    "\"id\":\"call_d\",\"type\":\"function\",\"index\":0,"
                    "\"function\":{\"name\":\"bash\",\"arguments\":\"{\\\"command\\\":\\\"l\"}}"
                    "]},\"index\":0}]}\n\n";
            // 第 2 帧:同 id,arguments 是接续片段 `s -la"}` —— 不是前缀超集。
            // 修复前会被识别为快照,acc.arguments 直接被替换成 `s -la"}`,丢掉前缀。
            body += "data: {\"choices\":[{\"delta\":{\"tool_calls\":[{"
                    "\"id\":\"call_d\",\"type\":\"function\",\"index\":0,"
                    "\"function\":{\"arguments\":\"s -la\\\"}\"}}"
                    "]},\"index\":0}]}\n\n";
            body += "data: {\"choices\":[{\"delta\":{},\"index\":0,\"finish_reason\":\"tool_calls\"}]}\n\n";
            body += "data: [DONE]\n\n";
            res.set_content(body, "text/event-stream");
            res.status = 200;
        });
    });

    OpenAiCompatProvider provider(
        "http://127.0.0.1:" + std::to_string(server.port), "", "test-model");

    ChatMessage user_msg;
    user_msg.role = "user";
    user_msg.content = "ls";
    std::vector<ChatMessage> messages = {user_msg};
    std::vector<ToolDef> tools;

    StreamCollector col;
    std::atomic<bool> abort_flag{false};
    provider.chat_stream(messages, tools, col.callback(), &abort_flag);

    ASSERT_EQ(col.tool_calls.size(), 1u);
    EXPECT_EQ(col.tool_calls[0].id, "call_d");
    EXPECT_EQ(col.tool_calls[0].function_name, "bash");
    // 关键:两段 delta 必须按拼接还原成完整 JSON,而不是只剩第 2 帧的尾段。
    EXPECT_EQ(col.tool_calls[0].function_arguments, R"({"command":"ls -la"})");
    auto parsed = nlohmann::json::parse(col.tool_calls[0].function_arguments,
                                         nullptr, false);
    ASSERT_FALSE(parsed.is_discarded());
    EXPECT_EQ(parsed.value("command", ""), "ls -la");
    EXPECT_EQ(col.error_events, 0);
}

// 用例 X3:GLM 风格逐帧增长的快照(不是固定的相同字符串重发,而是每一帧都更长)。
// 这是最接近真实 GLM 抓包形态的快照流:第 1 帧 `{"a"`,第 2 帧 `{"a":"b"}`。
// 必须仍走快照替换路径(因为后帧是前帧的前缀超集),最终只取最长的那份。
TEST(OpenAiProviderToolCallSnapshotTest, MonotonicGrowingSnapshotsStillReplaceCorrectly) {
    LocalHttpServer server([](httplib::Server& s) {
        s.Post("/chat/completions", [](const httplib::Request&, httplib::Response& res) {
            std::string body;
            // 第 1 帧:id + name + 部分快照 `{"a"`
            body += "data: {\"choices\":[{\"delta\":{\"tool_calls\":[{"
                    "\"id\":\"call_g\",\"type\":\"function\",\"index\":0,"
                    "\"function\":{\"name\":\"bash\",\"arguments\":\"{\\\"a\\\"\"}}"
                    "]},\"index\":0}]}\n\n";
            // 第 2 帧:同 id,完整快照 `{"a":"b"}` —— 是第 1 帧的前缀超集。
            body += "data: {\"choices\":[{\"delta\":{\"tool_calls\":[{"
                    "\"id\":\"call_g\",\"type\":\"function\",\"index\":0,"
                    "\"function\":{\"arguments\":\"{\\\"a\\\":\\\"b\\\"}\"}}"
                    "]},\"index\":0}]}\n\n";
            body += "data: {\"choices\":[{\"delta\":{},\"index\":0,\"finish_reason\":\"tool_calls\"}]}\n\n";
            body += "data: [DONE]\n\n";
            res.set_content(body, "text/event-stream");
            res.status = 200;
        });
    });

    OpenAiCompatProvider provider(
        "http://127.0.0.1:" + std::to_string(server.port), "", "test-model");

    ChatMessage user_msg;
    user_msg.role = "user";
    user_msg.content = "x";
    std::vector<ChatMessage> messages = {user_msg};
    std::vector<ToolDef> tools;

    StreamCollector col;
    std::atomic<bool> abort_flag{false};
    provider.chat_stream(messages, tools, col.callback(), &abort_flag);

    ASSERT_EQ(col.tool_calls.size(), 1u);
    // 关键:不应该被拼成 `{"a"{"a":"b"}`,也不应只剩第 1 帧片段。
    EXPECT_EQ(col.tool_calls[0].function_arguments, R"({"a":"b"})");
    EXPECT_EQ(col.error_events, 0);
}

// 用例 3:两个 tool_call 并发(不同 index)。GLM 风格也可能在同一帧/不同帧
// 同时携带多个 tool_call 全量快照,各自的 index 必须独立累积/替换,不能因
// 全量替换语义而互相污染。
TEST(OpenAiProviderToolCallSnapshotTest, MultipleIndicesAccumulateIndependently) {
    const std::string args0 = R"({"command":"ls"})";
    const std::string args1 = R"({"path":"/tmp/file.txt"})";

    LocalHttpServer server([&](httplib::Server& s) {
        s.Post("/chat/completions", [&](const httplib::Request&, httplib::Response& res) {
            // 第一帧:同时下发 index=0 和 index=1 两个 tool_call 的全量快照。
            // 第二帧:对两个 index 各自全量重发(GLM 行为)。
            auto make_chunk = [&]() {
                nlohmann::json j;
                nlohmann::json choice;
                choice["index"] = 0;
                choice["delta"]["tool_calls"] = nlohmann::json::array();
                {
                    nlohmann::json tc;
                    tc["id"] = "call_aaa";
                    tc["type"] = "function";
                    tc["index"] = 0;
                    tc["function"]["name"] = "bash";
                    tc["function"]["arguments"] = args0;
                    choice["delta"]["tool_calls"].push_back(tc);
                }
                {
                    nlohmann::json tc;
                    tc["id"] = "call_bbb";
                    tc["type"] = "function";
                    tc["index"] = 1;
                    tc["function"]["name"] = "file_read";
                    tc["function"]["arguments"] = args1;
                    choice["delta"]["tool_calls"].push_back(tc);
                }
                j["choices"].push_back(choice);
                return "data: " + j.dump() + "\n\n";
            };
            std::string body;
            body += make_chunk();
            body += make_chunk();
            body += "data: {\"choices\":[{\"delta\":{},\"index\":0,\"finish_reason\":\"tool_calls\"}]}\n\n";
            body += "data: [DONE]\n\n";
            res.set_content(body, "text/event-stream");
            res.status = 200;
        });
    });

    OpenAiCompatProvider provider(
        "http://127.0.0.1:" + std::to_string(server.port), "", "test-model");

    ChatMessage user_msg;
    user_msg.role = "user";
    user_msg.content = "x";
    std::vector<ChatMessage> messages = {user_msg};
    std::vector<ToolDef> tools;

    StreamCollector col;
    std::atomic<bool> abort_flag{false};
    provider.chat_stream(messages, tools, col.callback(), &abort_flag);

    ASSERT_EQ(col.tool_calls.size(), 2u);
    // pending_tools 是 std::map<int, ...>,flush 时按 index 升序遍历,
    // 因此 col.tool_calls[0] 必然是 index=0 那条。
    EXPECT_EQ(col.tool_calls[0].id, "call_aaa");
    EXPECT_EQ(col.tool_calls[0].function_name, "bash");
    EXPECT_EQ(col.tool_calls[0].function_arguments, args0);
    EXPECT_EQ(col.tool_calls[1].id, "call_bbb");
    EXPECT_EQ(col.tool_calls[1].function_name, "file_read");
    EXPECT_EQ(col.tool_calls[1].function_arguments, args1);
    EXPECT_EQ(col.error_events, 0);
}

} // namespace
