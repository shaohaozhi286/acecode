// 覆盖 src/provider/anthropic_provider.cpp 的 Anthropic Messages API 适配层。
//
// 场景:
//   - ACECode 内部 OpenAI 形态的 tool_calls/tool result 被转换成 Anthropic
//     system/messages/tools/tool_use/tool_result。
//   - 非流式响应解析 text/thinking/tool_use/usage。
//   - SSE 流解析 text_delta/input_json_delta/tool_use/usage/done。

#include <gtest/gtest.h>

#include "provider/anthropic_provider.hpp"
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
using acecode::AnthropicProvider;
using acecode::ChatMessage;
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

    std::string base_url() const {
        return "http://127.0.0.1:" + std::to_string(port);
    }
};

ChatMessage user_message(const std::string& content) {
    ChatMessage msg;
    msg.role = "user";
    msg.content = content;
    return msg;
}

} // namespace

TEST(AnthropicProviderTest, BuildRequestMapsSystemToolsAndToolResults) {
    AnthropicProvider provider(
        AnthropicProvider::kDefaultBaseUrl, "sk-ant-test", "claude-test");

    ChatMessage system;
    system.role = "system";
    system.content = "system rules";

    ChatMessage assistant;
    assistant.role = "assistant";
    assistant.content = "I'll read it.";
    assistant.tool_calls = nlohmann::json::array({
        {
            {"id", "toolu_1"},
            {"type", "function"},
            {"function", {
                {"name", "file_read"},
                {"arguments", R"({"path":"AGENTS.md"})"}
            }}
        }
    });

    ChatMessage tool;
    tool.role = "tool";
    tool.tool_call_id = "toolu_1";
    tool.content = "file contents";

    ToolDef def;
    def.name = "file_read";
    def.description = "Read a file";
    def.parameters = {
        {"type", "object"},
        {"properties", {{"path", {{"type", "string"}}}}},
        {"required", nlohmann::json::array({"path"})}
    };

    auto body = provider.build_request_body(
        {system, user_message("inspect"), assistant, tool}, {def}, true);

    EXPECT_EQ(body["model"], "claude-test");
    EXPECT_EQ(body["system"], "system rules");
    EXPECT_TRUE(body["stream"].get<bool>());
    ASSERT_EQ(body["messages"].size(), 3u);
    EXPECT_EQ(body["messages"][0]["role"], "user");
    EXPECT_EQ(body["messages"][0]["content"][0]["text"], "inspect");
    EXPECT_EQ(body["messages"][1]["role"], "assistant");
    EXPECT_EQ(body["messages"][1]["content"][1]["type"], "tool_use");
    EXPECT_EQ(body["messages"][1]["content"][1]["id"], "toolu_1");
    EXPECT_EQ(body["messages"][1]["content"][1]["name"], "file_read");
    EXPECT_EQ(body["messages"][1]["content"][1]["input"]["path"], "AGENTS.md");
    EXPECT_EQ(body["messages"][2]["role"], "user");
    EXPECT_EQ(body["messages"][2]["content"][0]["type"], "tool_result");
    EXPECT_EQ(body["messages"][2]["content"][0]["tool_use_id"], "toolu_1");
    ASSERT_EQ(body["tools"].size(), 1u);
    EXPECT_EQ(body["tools"][0]["name"], "file_read");
    EXPECT_EQ(body["tools"][0]["input_schema"]["type"], "object");
}

TEST(AnthropicProviderTest, ParseResponseMapsContentUsageAndToolUse) {
    nlohmann::json response = {
        {"stop_reason", "tool_use"},
        {"usage", {
            {"input_tokens", 11},
            {"output_tokens", 7},
            {"cache_read_input_tokens", 3},
            {"cache_creation_input_tokens", 2}
        }},
        {"content", nlohmann::json::array({
            {{"type", "thinking"}, {"thinking", "plan"}},
            {{"type", "text"}, {"text", "Need a file."}},
            {
                {"type", "tool_use"},
                {"id", "toolu_1"},
                {"name", "file_read"},
                {"input", {{"path", "AGENTS.md"}}}
            }
        })}
    };

    auto parsed = AnthropicProvider::parse_response(response);

    EXPECT_EQ(parsed.finish_reason, "tool_use");
    EXPECT_EQ(parsed.reasoning_content, "plan");
    EXPECT_EQ(parsed.content, "Need a file.");
    ASSERT_EQ(parsed.tool_calls.size(), 1u);
    EXPECT_EQ(parsed.tool_calls[0].id, "toolu_1");
    EXPECT_EQ(parsed.tool_calls[0].function_name, "file_read");
    EXPECT_EQ(nlohmann::json::parse(parsed.tool_calls[0].function_arguments)["path"],
              "AGENTS.md");
    EXPECT_TRUE(parsed.usage.has_data);
    EXPECT_EQ(parsed.usage.prompt_tokens, 11);
    EXPECT_EQ(parsed.usage.completion_tokens, 7);
    EXPECT_EQ(parsed.usage.total_tokens, 18);
    EXPECT_EQ(parsed.usage.cache_read_tokens, 3);
    EXPECT_EQ(parsed.usage.cache_write_tokens, 2);
}

TEST(AnthropicProviderTest, MissingApiKeyFailsBeforeNetwork) {
    std::atomic<int> hits{0};
    LocalHttpServer server([&](httplib::Server& s) {
        s.Post("/messages", [&](const httplib::Request&, httplib::Response& res) {
            ++hits;
            res.set_content(R"({"content":[]})", "application/json");
            res.status = 200;
        });
    });

    AnthropicProvider provider(server.base_url(), "", "claude-test", 5000);

    auto response = provider.chat({user_message("hi")}, {});
    EXPECT_EQ(response.finish_reason, "error");
    EXPECT_NE(response.content.find("missing Anthropic API key"), std::string::npos);
    EXPECT_EQ(hits.load(), 0);

    int error_events = 0;
    ProviderErrorKind kind = ProviderErrorKind::None;
    std::string error;
    provider.chat_stream({user_message("hi")}, {}, [&](const StreamEvent& evt) {
        if (evt.type != StreamEventType::Error) return;
        ++error_events;
        kind = evt.provider_error.kind;
        error = evt.error;
    });

    EXPECT_EQ(error_events, 1);
    EXPECT_EQ(kind, ProviderErrorKind::Unknown);
    EXPECT_NE(error.find("missing Anthropic API key"), std::string::npos);
    EXPECT_EQ(hits.load(), 0);
}

TEST(AnthropicProviderTest, StreamParsesTextToolUseUsageAndDone) {
    std::mutex mu;
    std::string seen_api_key;
    std::string seen_custom_header;
    std::string seen_body;

    LocalHttpServer server([&](httplib::Server& s) {
        s.Post("/messages", [&](const httplib::Request& req, httplib::Response& res) {
            {
                std::lock_guard<std::mutex> lk(mu);
                seen_api_key = req.get_header_value("x-api-key");
                seen_custom_header = req.get_header_value("X-Trace");
                seen_body = req.body;
            }
            std::string body =
                "event: message_start\n"
                "data: {\"type\":\"message_start\",\"message\":{\"usage\":{\"input_tokens\":11,\"output_tokens\":0}}}\n\n"
                "event: content_block_start\n"
                "data: {\"type\":\"content_block_start\",\"index\":0,\"content_block\":{\"type\":\"text\",\"text\":\"\"}}\n\n"
                "event: content_block_delta\n"
                "data: {\"type\":\"content_block_delta\",\"index\":0,\"delta\":{\"type\":\"text_delta\",\"text\":\"hi\"}}\n\n"
                "event: content_block_stop\n"
                "data: {\"type\":\"content_block_stop\",\"index\":0}\n\n"
                "event: content_block_start\n"
                "data: {\"type\":\"content_block_start\",\"index\":1,\"content_block\":{\"type\":\"tool_use\",\"id\":\"toolu_1\",\"name\":\"file_read\",\"input\":{}}}\n\n"
                "event: content_block_delta\n"
                "data: {\"type\":\"content_block_delta\",\"index\":1,\"delta\":{\"type\":\"input_json_delta\",\"partial_json\":\"{\\\"path\\\":\\\"AGENTS.md\\\"}\"}}\n\n"
                "event: content_block_stop\n"
                "data: {\"type\":\"content_block_stop\",\"index\":1}\n\n"
                "event: message_delta\n"
                "data: {\"type\":\"message_delta\",\"delta\":{\"stop_reason\":\"tool_use\",\"stop_sequence\":null},\"usage\":{\"output_tokens\":5}}\n\n"
                "event: message_stop\n"
                "data: {\"type\":\"message_stop\"}\n\n";
            res.set_content(body, "text/event-stream");
            res.status = 200;
        });
    });

    AnthropicProvider provider(
        server.base_url(), "sk-ant-test", "claude-test", 5000,
        {{"X-Trace", "acecode"}});

    std::string text;
    std::vector<acecode::ToolCall> tool_calls;
    int deltas = 0;
    int tool_call_deltas = 0;
    int usage_events = 0;
    int done_events = 0;
    int error_events = 0;
    std::string finish_reason;
    acecode::TokenUsage usage;
    std::mutex events_mu;

    auto cb = [&](const StreamEvent& evt) {
        std::lock_guard<std::mutex> lk(events_mu);
        switch (evt.type) {
            case StreamEventType::Delta:
                text += evt.content;
                ++deltas;
                break;
            case StreamEventType::ToolCallDelta:
                ++tool_call_deltas;
                break;
            case StreamEventType::ToolCall:
                tool_calls.push_back(evt.tool_call);
                break;
            case StreamEventType::Usage:
                usage = evt.usage;
                ++usage_events;
                break;
            case StreamEventType::Done:
                finish_reason = evt.finish_reason;
                ++done_events;
                break;
            case StreamEventType::Error:
                ++error_events;
                break;
            default:
                break;
        }
    };

    std::atomic<bool> abort_flag{false};
    provider.chat_stream({user_message("hi")}, {}, cb, &abort_flag);

    EXPECT_EQ(text, "hi");
    EXPECT_EQ(deltas, 1);
    EXPECT_EQ(tool_call_deltas, 1);
    ASSERT_EQ(tool_calls.size(), 1u);
    EXPECT_EQ(tool_calls[0].id, "toolu_1");
    EXPECT_EQ(tool_calls[0].function_name, "file_read");
    EXPECT_EQ(nlohmann::json::parse(tool_calls[0].function_arguments)["path"], "AGENTS.md");
    EXPECT_EQ(usage_events, 1);
    EXPECT_TRUE(usage.has_data);
    EXPECT_EQ(usage.prompt_tokens, 11);
    EXPECT_EQ(usage.completion_tokens, 5);
    EXPECT_EQ(done_events, 1);
    EXPECT_EQ(finish_reason, "tool_use");
    EXPECT_EQ(error_events, 0);

    std::lock_guard<std::mutex> lk(mu);
    EXPECT_EQ(seen_api_key, "sk-ant-test");
    EXPECT_EQ(seen_custom_header, "acecode");
    auto request = nlohmann::json::parse(seen_body);
    EXPECT_TRUE(request["stream"].get<bool>());
    EXPECT_EQ(request["model"], "claude-test");
}
