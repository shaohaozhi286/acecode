#include <gtest/gtest.h>

#include "agent_loop.hpp"
#include "hooks/hook_config.hpp"
#include "hooks/hook_manager.hpp"
#include "permissions.hpp"
#include "tool/tool_executor.hpp"
#include "../agent_loop/stub_provider.hpp"

#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <vector>

using namespace std::chrono_literals;

TEST(HookAgentLoop, DispatchesAssistantCompletedAfterTextMessageCommit) {
    auto provider = std::make_shared<acecode_test::StubLlmProvider>();
    provider->push_text("done");

    acecode::HookConfig hook_cfg;
    hook_cfg.enabled = true;
    acecode::HookDefinition hook;
    hook.id = "capture";
    hook.event = acecode::kHookEventAssistantMessageCompleted;
    hook.mode = acecode::HookMode::Sync;
    hook.command.command = "noop";
    hook_cfg.events[acecode::kHookEventAssistantMessageCompleted].push_back(hook);

    std::mutex mu;
    std::condition_variable cv;
    std::vector<nlohmann::json> payloads;
    acecode::HookManager hooks(std::move(hook_cfg),
        [&](const acecode::HookCommandSpec&,
            const std::string& stdin_text,
            int,
            const std::string&) {
            {
                std::lock_guard<std::mutex> lk(mu);
                payloads.push_back(nlohmann::json::parse(stdin_text));
            }
            cv.notify_all();
            acecode::HookProcessResult r;
            r.started = true;
            r.exit_code = 0;
            return r;
        });

    acecode::ToolExecutor tools;
    acecode::PermissionManager permissions;
    bool busy = false;
    acecode::AgentCallbacks callbacks;
    callbacks.on_busy_changed = [&](bool value) {
        {
            std::lock_guard<std::mutex> lk(mu);
            busy = value;
        }
        cv.notify_all();
    };

    acecode::AgentLoop loop(
        [&provider]() -> std::shared_ptr<acecode::LlmProvider> { return provider; },
        tools,
        callbacks,
        ".",
        permissions);
    loop.set_hook_manager(&hooks);
    loop.submit("hello");

    std::unique_lock<std::mutex> lk(mu);
    ASSERT_TRUE(cv.wait_for(lk, 5s, [&] {
        return !busy && !payloads.empty();
    }));
    ASSERT_EQ(payloads.size(), 1u);
    EXPECT_EQ(payloads[0]["event"], acecode::kHookEventAssistantMessageCompleted);
    EXPECT_EQ(payloads[0]["hook_id"], "capture");
    EXPECT_EQ(payloads[0]["assistant"]["kind"], "text");
    EXPECT_EQ(payloads[0]["assistant"]["content"], "done");
    EXPECT_EQ(payloads[0]["model"]["provider"], "stub");
    EXPECT_EQ(payloads[0]["model"]["model"], "stub-1");
}

