// 空回复兜底的全链路集成测试(fix-glm-empty-response-turn-end)。
//
// 与 agent_loop_termination_test.cpp 的 stub 用例不同,这里用真实的
// OpenAiCompatProvider 打到本地 httplib mock server,覆盖完整接缝:
//   SSE 字节流 → provider 解析(reasoning_content / finish_reason)
//   → Done 事件透传 finish_reason → AgentLoop 空回复兜底
//   → 注入 hidden 重试提示 → 第二次 HTTP 请求(含 reasoning 回传)→ 恢复。
//
// 回归背景(用户反馈会话 20260703-022813-6f8f,火山引擎 GLM):模型深度思考
// 耗尽输出 token 预算,服务端返回 HTTP 200 + 仅 reasoning 的 SSE + [DONE],
// 旧行为静默终止回合,用户反馈"没有完成任务就停止了"。本文件用 mock 精确
// 复刻该线上响应形态,断言新行为自动重试并恢复。

#include <gtest/gtest.h>

#include "agent_loop.hpp"
#include "permissions.hpp"
#include "provider/llm_provider.hpp"
#include "provider/openai_provider.hpp"
#include "tool/tool_executor.hpp"

#include <httplib.h>
#include <nlohmann/json.hpp>

#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace std::chrono_literals;

// 本地 mock server:第 1 次请求回「仅思考 + finish_reason=length」(火山 GLM
// 截断形态),之后的请求回正常文本。所有请求 body 存档供断言。
struct MockGlmServer {
    httplib::Server svr;
    int port = 0;
    std::thread th;
    std::mutex mu;
    std::vector<nlohmann::json> request_bodies;

    MockGlmServer() {
        svr.Post("/chat/completions",
                 [this](const httplib::Request& req, httplib::Response& res) {
            int request_index = 0;
            {
                std::lock_guard<std::mutex> lk(mu);
                request_bodies.push_back(nlohmann::json::parse(req.body));
                request_index = static_cast<int>(request_bodies.size());
            }
            std::string body;
            if (request_index == 1) {
                // 复刻线上形态:一大段 reasoning、零 content、最后一个 chunk
                // 上报 finish_reason=length,然后正常 [DONE] 收尾。
                body =
                    "data: {\"choices\":[{\"delta\":{\"reasoning_content\":"
                    "\"think hard... budget exhausted\"}}]}\n\n"
                    "data: {\"choices\":[{\"delta\":{},\"finish_reason\":\"length\"}]}\n\n"
                    "data: [DONE]\n\n";
            } else {
                body =
                    "data: {\"choices\":[{\"delta\":{\"content\":\"recovered answer\"},"
                    "\"finish_reason\":\"stop\"}]}\n\n"
                    "data: [DONE]\n\n";
            }
            res.set_content(body, "text/event-stream");
            res.status = 200;
        });
        port = svr.bind_to_any_port("127.0.0.1");
        th = std::thread([this] { svr.listen_after_bind(); });
        for (int i = 0; i < 50 && !svr.is_running(); ++i) {
            std::this_thread::sleep_for(10ms);
        }
    }

    ~MockGlmServer() {
        svr.stop();
        if (th.joinable()) th.join();
    }

    int request_count() {
        std::lock_guard<std::mutex> lk(mu);
        return static_cast<int>(request_bodies.size());
    }

    nlohmann::json request_body(int zero_based_index) {
        std::lock_guard<std::mutex> lk(mu);
        if (zero_based_index < 0 ||
            static_cast<std::size_t>(zero_based_index) >= request_bodies.size()) {
            return nlohmann::json{};
        }
        return request_bodies[static_cast<std::size_t>(zero_based_index)];
    }
};

// 场景:真实 provider 收到「仅思考被 length 截断」的空回复 → AgentLoop 注入
// 重试提示并重发请求 → mock 第 2 次回正常文本 → 回合正常完成。
// 关键断言:
//   1. mock 恰好收到 2 次 HTTP 请求(1 空 + 1 重试),无多余轮次;
//   2. 第 2 次请求里带注入的 [SYSTEM NOTE] 提示,且点明 finish_reason=length;
//   3. 第 2 次请求把上一轮空 assistant 的 reasoning_content 回传(DeepSeek/GLM
//      thinking 模式的协议要求,丢了会 400);
//   4. 最终 dispatch 的 assistant 是恢复后的 "recovered answer",无 error。
TEST(AgentLoopEmptyResponseIntegration, GlmLengthTruncationRetriesOverRealHttpAndRecovers) {
    MockGlmServer server;
    auto provider = std::make_shared<acecode::OpenAiCompatProvider>(
        "http://127.0.0.1:" + std::to_string(server.port), "", "mock-glm");

    acecode::ToolExecutor tools;
    acecode::PermissionManager permissions;

    std::mutex mu;
    std::condition_variable cv;
    bool busy = false;
    std::vector<std::pair<std::string, std::string>> dispatched; // (role, content)

    acecode::AgentCallbacks callbacks;
    callbacks.on_message = [&](const std::string& role,
                               const std::string& content, bool) {
        std::lock_guard<std::mutex> lk(mu);
        dispatched.emplace_back(role, content);
    };
    callbacks.on_busy_changed = [&](bool value) {
        std::lock_guard<std::mutex> lk(mu);
        busy = value;
        if (!busy) cv.notify_all();
    };

    acecode::AgentLoop loop(
        [provider]() -> std::shared_ptr<acecode::LlmProvider> { return provider; },
        tools, callbacks, ".", permissions);

    {
        std::lock_guard<std::mutex> lk(mu);
        busy = true;
    }
    loop.submit("analyze the project");
    {
        std::unique_lock<std::mutex> lk(mu);
        ASSERT_TRUE(cv.wait_for(lk, 15s, [&] { return !busy; }));
    }

    // 1) 恰好 2 次请求:空回复 1 次 + 重试 1 次,恢复后立即收敛。
    EXPECT_EQ(server.request_count(), 2);

    // 2) + 3) 第 2 次请求:注入提示进入 messages,且空 assistant 的 reasoning
    //          被回传。
    const auto second = server.request_body(1);
    ASSERT_TRUE(second.contains("messages"));
    bool saw_retry_prompt = false;
    bool saw_reasoning_echo = false;
    for (const auto& msg : second["messages"]) {
        const std::string role = msg.value("role", "");
        const std::string content = msg.value("content", "");
        if (role == "user" &&
            content.find("[SYSTEM NOTE]") != std::string::npos &&
            content.find("finish_reason=length") != std::string::npos) {
            saw_retry_prompt = true;
        }
        if (role == "assistant" &&
            msg.value("reasoning_content", std::string{})
                    .find("budget exhausted") != std::string::npos) {
            saw_reasoning_echo = true;
        }
    }
    EXPECT_TRUE(saw_retry_prompt);
    EXPECT_TRUE(saw_reasoning_echo);

    // 4) 最终结果:恢复后的正文被 dispatch,全程无 error;空回复不产生
    //    assistant 气泡(只有恢复轮的一条)。
    std::lock_guard<std::mutex> lk(mu);
    int assistant_count = 0;
    int error_count = 0;
    std::string last_assistant;
    bool saw_empty_notice = false;
    for (const auto& [role, content] : dispatched) {
        if (role == "assistant") {
            ++assistant_count;
            last_assistant = content;
        }
        if (role == "error") ++error_count;
        if (role == "system" &&
            content.find(u8"[空回复]") != std::string::npos) {
            saw_empty_notice = true;
        }
    }
    EXPECT_EQ(assistant_count, 1);
    EXPECT_EQ(last_assistant, "recovered answer");
    EXPECT_EQ(error_count, 0);
    EXPECT_TRUE(saw_empty_notice);
}

} // namespace
