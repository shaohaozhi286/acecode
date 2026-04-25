// 端到端测试 AgentLoop 终止协议(openspec/changes/align-loop-with-hermes):
//   (a) text-only 响应直接结束 loop,无条件
//   (b) turn 1 调用 task_complete → 1 轮退出,UI 渲染 Done 摘要
//   (c) 长链工具调用 → 命中 max_iterations 硬上限
//   (d) AskUserQuestion 不是终止器 — 模型应继续下一轮(tool_result 走回模型)
//   (e) 用户 abort → 立刻退出,发 [Interrupted] 系统消息
//
// 关键机制:
// - 用 StubLlmProvider 脚本化每轮 LLM 响应
// - 用 on_busy_changed(false) + cv 同步测试主线程
// - 用 on_message 收集所有消息流,断言数量和内容
//
// AgentLoop 的 worker_thread 会在构造时启动,在析构(shutdown)时 join。
// 测试每个用例构造一个独立的 AgentLoop 实例,用 RAII 确保清理。

#include <gtest/gtest.h>

#include "agent_loop.hpp"
#include "stub_provider.hpp"
#include "tool/task_complete_tool.hpp"
#include "tool/tool_executor.hpp"
#include "permissions.hpp"
#include "provider/llm_provider.hpp"

#include <nlohmann/json.hpp>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using acecode::AgentLoop;
using acecode::AgentCallbacks;
using acecode::ChatMessage;
using acecode::PermissionManager;
using acecode::PermissionResult;
using acecode::ToolDef;
using acecode::ToolExecutor;
using acecode::ToolImpl;
using acecode::ToolResult;
using acecode::ToolSource;
using acecode_test::ScriptedResponse;
using acecode_test::StubLlmProvider;

namespace {

// 一个零副作用的占位"noop"工具,用于让长链工具调用走通(测 max_iterations 用)。
ToolImpl create_noop_tool() {
    ToolDef def;
    def.name = "noop";
    def.description = "Stub tool for agent-loop tests; returns success immediately.";
    def.parameters = {
        {"type", "object"},
        {"properties", nlohmann::json::object()}
    };
    ToolImpl impl;
    impl.definition = def;
    impl.execute = [](const std::string&, const acecode::ToolContext&) {
        return ToolResult{"ok", true};
    };
    impl.is_read_only = true;  // 避免触发 permission 确认
    impl.source = ToolSource::Builtin;
    return impl;
}

// Fixture:封装 AgentLoop + stub + 消息收集器 + 完成同步。
class AgentLoopHarness {
public:
    AgentLoopHarness() {
        tools_.register_tool(create_noop_tool());
        tools_.register_tool(acecode::create_task_complete_tool());

        AgentCallbacks cb;
        cb.on_message = [this](const std::string& role,
                               const std::string& content, bool is_tool) {
            std::lock_guard<std::mutex> lk(msg_mu_);
            messages_.push_back({role, content, is_tool});
        };
        cb.on_busy_changed = [this](bool busy) {
            std::lock_guard<std::mutex> lk(busy_mu_);
            is_busy_ = busy;
            if (!busy) busy_cv_.notify_all();
        };
        cb.on_tool_confirm = [](const std::string&, const std::string&) {
            return PermissionResult::Allow;
        };

        auto provider_accessor =
            [this]() -> std::shared_ptr<acecode::LlmProvider> { return provider_; };

        loop_ = std::make_unique<AgentLoop>(
            provider_accessor, tools_, cb, /*cwd=*/".", perms_);
    }

    ~AgentLoopHarness() {
        // AgentLoop::shutdown() 由 dtor 调用,会 join worker thread。
        // 不需要显式做 —— RAII 保证。
    }

    void set_config(acecode::AgentLoopConfig cfg) {
        loop_->set_agent_loop_config(cfg);
    }

    void set_stub_latency_ms(int ms) { provider_->set_latency_ms(ms); }

    void push_text(std::string s) { provider_->push_text(std::move(s)); }
    void push_tool_call(std::string name, std::string args, std::string id = "c1") {
        provider_->push_tool_call(std::move(name), std::move(args), std::move(id));
    }
    void push_task_complete(std::string summary, std::string id = "c-done") {
        nlohmann::json args = {{"summary", std::move(summary)}};
        provider_->push_tool_call("task_complete", args.dump(), std::move(id));
    }

    // 发消息并阻塞直到 on_busy_changed(false)。返回 false 代表超时(测试失败信号)。
    bool submit_and_wait(const std::string& msg,
                        std::chrono::milliseconds timeout = std::chrono::seconds(5)) {
        {
            std::lock_guard<std::mutex> lk(busy_mu_);
            is_busy_ = true;  // 认定 submit 前就进入 busy 状态;on_busy_changed 会先升后降
        }
        loop_->submit(msg);
        std::unique_lock<std::mutex> lk(busy_mu_);
        return busy_cv_.wait_for(lk, timeout, [this] { return !is_busy_; });
    }

    void abort() { loop_->abort(); }

    int turn_count() const { return provider_->turn_count(); }

    struct Msg {
        std::string role;
        std::string content;
        bool is_tool = false;
    };

    std::vector<Msg> snapshot_messages() {
        std::lock_guard<std::mutex> lk(msg_mu_);
        return messages_;
    }

    int count_by_role(const std::string& role) {
        std::lock_guard<std::mutex> lk(msg_mu_);
        int n = 0;
        for (const auto& m : messages_) if (m.role == role) ++n;
        return n;
    }

    // align-loop-with-hermes:loop 不再注入 nudge;此 helper 仅作为防回归断言,
    // 任何包含 [acecode:auto-continue] 前缀的 user 消息都说明回归了 nudge 路径。
    int count_nudges() {
        std::lock_guard<std::mutex> lk(msg_mu_);
        int n = 0;
        for (const auto& m : messages_) {
            if (m.role == "user" &&
                m.content.find("[acecode:auto-continue]") != std::string::npos) {
                ++n;
            }
        }
        return n;
    }

    // 找到最后一条 role=system 的消息(诊断停机原因)。
    std::string last_system_message() {
        std::lock_guard<std::mutex> lk(msg_mu_);
        for (auto it = messages_.rbegin(); it != messages_.rend(); ++it) {
            if (it->role == "system") return it->content;
        }
        return {};
    }

private:
    std::shared_ptr<StubLlmProvider> provider_ = std::make_shared<StubLlmProvider>();
    ToolExecutor tools_;
    PermissionManager perms_;
    std::unique_ptr<AgentLoop> loop_;

    std::mutex msg_mu_;
    std::vector<Msg> messages_;

    std::mutex busy_mu_;
    std::condition_variable busy_cv_;
    bool is_busy_ = false;
};

} // namespace

// 场景 (a):text-only 响应直接结束 loop。chit-chat 与 mid-task hedge 都走这条路径。
TEST(AgentLoopTermination, TextOnlyEndsTurnUnconditionally) {
    AgentLoopHarness h;
    h.push_text("你好!有什么可以帮你的?");

    ASSERT_TRUE(h.submit_and_wait("你好"));
    EXPECT_EQ(h.turn_count(), 1);
    EXPECT_EQ(h.count_nudges(), 0);  // 防回归:绝不该出现 nudge
    EXPECT_EQ(h.last_system_message().find("Agent loop stopped"),
              std::string::npos);  // 正常退出,无 cap 消息
}

// 场景 (b):turn 1 就调用 task_complete → 1 轮退出,无 cap 消息
TEST(AgentLoopTermination, TaskCompleteTerminatesImmediately) {
    AgentLoopHarness h;
    h.push_task_complete("done in one turn");

    ASSERT_TRUE(h.submit_and_wait("do something"));
    EXPECT_EQ(h.turn_count(), 1);
    EXPECT_EQ(h.count_nudges(), 0);
    EXPECT_EQ(h.last_system_message().find("Agent loop stopped"),
              std::string::npos);
}

// 场景 (c):max_iterations 硬上限触发
TEST(AgentLoopTermination, MaxIterationsHardCap) {
    AgentLoopHarness h;
    acecode::AgentLoopConfig cfg;
    cfg.max_iterations = 3;
    h.set_config(cfg);

    // 全部 tool-call,保证 loop 不在 text-only 分支提前退出
    for (int i = 0; i < 10; ++i) {
        h.push_tool_call("noop", "{}", "c" + std::to_string(i));
    }

    ASSERT_TRUE(h.submit_and_wait("do it"));
    EXPECT_EQ(h.turn_count(), 3);
    EXPECT_NE(h.last_system_message().find("max_iterations"),
              std::string::npos);
}

// 场景 (d):AskUserQuestion 不是终止器 —— tool_result 回模型后 loop 应该继续。
// 模拟流程:模型第 1 轮调 AskUserQuestion → tool 未注册返回 "Unknown tool"
// → 第 2 轮模型看到 tool_result,调 task_complete → 退出。
// 关键断言:turn_count == 2(loop 没在 AskUserQuestion 这轮就结束)。
TEST(AgentLoopTermination, AskUserQuestionDoesNotTerminate) {
    AgentLoopHarness h;
    h.push_tool_call("AskUserQuestion", R"({"questions":[]})", "ask-1");
    h.push_task_complete("acknowledged");

    ASSERT_TRUE(h.submit_and_wait("do it"));
    EXPECT_EQ(h.turn_count(), 2);
    EXPECT_EQ(h.count_nudges(), 0);
}

// 场景 (e):用户 abort 立刻生效。让 stub 的 chat_stream 阻塞 ~200ms 轮询
// abort_flag,给主线程一个确定性窗口下 abort。
TEST(AgentLoopTermination, UserAbortShortCircuits) {
    AgentLoopHarness h;
    acecode::AgentLoopConfig cfg;
    cfg.max_iterations = 50;
    h.set_config(cfg);
    h.set_stub_latency_ms(200);  // 第一轮 chat_stream 至少停 200ms 轮询 abort

    // 全 tool_call 让 loop 反复转,abort 必须能截停
    for (int i = 0; i < 10; ++i) h.push_tool_call("noop", "{}", "c" + std::to_string(i));

    std::thread t([&h]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        h.abort();
    });
    ASSERT_TRUE(h.submit_and_wait("do it", std::chrono::seconds(10)));
    t.join();

    // Abort 不应触发 max_iterations cap 信息
    const std::string last = h.last_system_message();
    EXPECT_EQ(last.find("max_iterations"), std::string::npos);
    // 也绝不该累积 nudge
    EXPECT_EQ(h.count_nudges(), 0);
}
