// 覆盖 src/provider/openai_provider.cpp 中 SSE "data:" 前缀的兼容性解析。
//
// 背景：HTML5 EventSource 规范(SSE)允许 "data:" 字段名后的单个前导空格可选,
//   即 "data: foo" 与 "data:foo" 都是合法的。OpenAI / DeepSeek 等主流服务
//   按惯例使用前者,但有些自建网关 —— 例如平安 wizard-ai 的 minimax 系
//   (实际抓包样本: data:{"created":...,"model":"minimax-m2.5",...} ) ——
//   发的是无空格前缀。早期实现严格匹配 "data: " 会把整条流当作空响应丢弃,
//   表现为用户感知到的"消息发出去后立即终止、无任何返回",但 fiddler
//   抓包显示服务器其实正常推送了 chunk。这里固化两种前缀都必须被识别。
//
// 本文件覆盖的场景:
//   1. 无空格前缀 "data:{...}" 的 SSE 流能正确累积 delta.content。
//   2. 同一流里混用有空格 "data: {...}" 与无空格 "data:{...}" 也能正常解析。
//   3. 无空格的 "data:[DONE]" 终止信号也能被识别(并触发 Done 事件)。

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
using acecode::StreamEvent;
using acecode::StreamEventType;
using acecode::ToolDef;

// 启动一个本地 httplib server,析构时自动 stop。与 reasoning 测试里的
// LocalHttpServer 同形,这里独立维护避免跨文件链接耦合。
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

// 用例 1:无空格 "data:{...}" 前缀的 SSE 流,delta.content 必须能累积出来。
//
// 这是平安 wizard-ai 的实际抓包形态。修复前所有 chunk 会被静默丢弃,
// aggregated_content 为空、Done 事件不触发,表现为"无响应直接终止"。
TEST(OpenAiProviderSsePrefixTest, SseAcceptsNoSpaceDataPrefix) {
    LocalHttpServer server([](httplib::Server& s) {
        s.Post("/chat/completions", [](const httplib::Request&, httplib::Response& res) {
            // 关键:每个 "data:" 后立即跟 JSON,无空格分隔。
            std::string body =
                "data:{\"choices\":[{\"delta\":{\"content\":\"hel\"}}]}\n\n"
                "data:{\"choices\":[{\"delta\":{\"content\":\"lo\"},\"finish_reason\":\"stop\"}]}\n\n"
                "data:[DONE]\n\n";
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

    std::string aggregated_content;
    int delta_events = 0, done_events = 0, error_events = 0;
    std::mutex mu;
    auto cb = [&](const StreamEvent& evt) {
        std::lock_guard<std::mutex> lk(mu);
        switch (evt.type) {
            case StreamEventType::Delta: aggregated_content += evt.content; ++delta_events; break;
            case StreamEventType::Done:  ++done_events; break;
            case StreamEventType::Error: ++error_events; break;
            default: break;
        }
    };

    std::atomic<bool> abort_flag{false};
    provider.chat_stream(messages, tools, cb, &abort_flag);

    EXPECT_EQ(aggregated_content, "hello");
    EXPECT_EQ(delta_events, 2);
    EXPECT_EQ(done_events, 1);
    EXPECT_EQ(error_events, 0);
}

// 用例 2:有空格与无空格前缀混用,解析器都要正确处理,且不会把空格当作 JSON
// 的一部分(否则 nlohmann::json::parse 会在前导空格上 noop 通过,但若实现
// 误剥离两个字符就会破坏第一个字节,引发 parse_error)。
TEST(OpenAiProviderSsePrefixTest, SseAcceptsMixedSpaceAndNoSpacePrefixes) {
    LocalHttpServer server([](httplib::Server& s) {
        s.Post("/chat/completions", [](const httplib::Request&, httplib::Response& res) {
            std::string body =
                "data: {\"choices\":[{\"delta\":{\"content\":\"A\"}}]}\n\n"
                "data:{\"choices\":[{\"delta\":{\"content\":\"B\"}}]}\n\n"
                "data: {\"choices\":[{\"delta\":{\"content\":\"C\"},\"finish_reason\":\"stop\"}]}\n\n"
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

    std::string aggregated_content;
    int delta_events = 0, done_events = 0, error_events = 0;
    std::mutex mu;
    auto cb = [&](const StreamEvent& evt) {
        std::lock_guard<std::mutex> lk(mu);
        switch (evt.type) {
            case StreamEventType::Delta: aggregated_content += evt.content; ++delta_events; break;
            case StreamEventType::Done:  ++done_events; break;
            case StreamEventType::Error: ++error_events; break;
            default: break;
        }
    };

    std::atomic<bool> abort_flag{false};
    provider.chat_stream(messages, tools, cb, &abort_flag);

    EXPECT_EQ(aggregated_content, "ABC");
    EXPECT_EQ(delta_events, 3);
    EXPECT_EQ(done_events, 1);
    EXPECT_EQ(error_events, 0);
}

} // namespace
