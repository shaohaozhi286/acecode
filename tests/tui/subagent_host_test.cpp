// 覆盖 src/tui/subagent_host.cpp。
//
// SubagentHost 是 TUI 的子代理宿主:进程内 SessionRegistry + 事件桥接 +
// 右侧「运行中任务」快照 + /tasks 的 list/abort/clear 后端。一旦回归:
//   - publish 快照失灵 → 右侧栏不出现/不消失(wait=false 的任务无感知)
//   - busy=false 不移除 → 「只显示运行中」的用户决策被打破
//   - clear_settled 误删运行中任务 → 数据丢失
//   - permission_request 不冒泡 → default 模式子代理卡 5 分钟超时
//
// 测试不真跑 LLM:EchoStreamProvider 立即完成 turn,让 busy 迁移与消息
// 落盘真实发生。

#include <gtest/gtest.h>

#include "config/config.hpp"
#include "permissions.hpp"
#include "session/session_storage.hpp"
#include "tool/tool_executor.hpp"
#include "tui/subagent_host.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <random>
#include <thread>

using namespace std::chrono_literals;
namespace fs = std::filesystem;

namespace {

class EchoStreamProvider : public acecode::LlmProvider {
public:
    acecode::ChatResponse chat(
        const std::vector<acecode::ChatMessage>&,
        const std::vector<acecode::ToolDef>&) override {
        acecode::ChatResponse resp;
        resp.content = "subagent-final-reply";
        resp.finish_reason = "stop";
        return resp;
    }
    void chat_stream(const std::vector<acecode::ChatMessage>&,
                     const std::vector<acecode::ToolDef>&,
                     const acecode::StreamCallback& callback,
                     std::atomic<bool>* = nullptr) override {
        acecode::StreamEvent delta;
        delta.type = acecode::StreamEventType::Delta;
        delta.content = "subagent-final-reply";
        callback(delta);
        acecode::StreamEvent done;
        done.type = acecode::StreamEventType::Done;
        callback(done);
    }
    std::string name() const override { return "echo-stub"; }
    bool is_authenticated() override { return true; }
    std::string model() const override { return "echo-stub"; }
    void set_model(const std::string&) override {}
};

struct HostFixture {
    acecode::ToolExecutor tools;
    acecode::PermissionManager permissions;
    std::shared_ptr<acecode::LlmProvider> provider =
        std::make_shared<EchoStreamProvider>();
    fs::path cwd;
    std::string parent_id = "20260705-000000-aaaa";

    std::mutex mu;
    std::vector<acecode::tui::SubagentTaskSnapshot> published;
    std::vector<std::string> permission_events;

    std::unique_ptr<acecode::tui::SubagentHost> host;

    HostFixture() {
        std::random_device rd;
        cwd = fs::temp_directory_path() /
              ("acecode_subagent_host_" + std::to_string(rd()));
        fs::create_directories(cwd);

        acecode::tui::SubagentHost::Deps deps;
        deps.registry_deps.provider_accessor = [this] { return provider; };
        deps.registry_deps.tools = &tools;
        deps.registry_deps.cwd = cwd.string();
        deps.registry_deps.template_permissions = &permissions;
        deps.parent_session_id = [this] { return parent_id; };
        deps.publish_tasks = [this](auto tasks) {
            std::lock_guard<std::mutex> lk(mu);
            published = std::move(tasks);
        };
        deps.on_permission_request = [this](const std::string& sid,
                                            const std::string&,
                                            nlohmann::json) {
            std::lock_guard<std::mutex> lk(mu);
            permission_events.push_back(sid);
        };
        host = std::make_unique<acecode::tui::SubagentHost>(std::move(deps));
    }

    ~HostFixture() {
        host.reset();
        std::error_code ec;
        fs::remove_all(cwd, ec);
    }

    std::size_t published_count() {
        std::lock_guard<std::mutex> lk(mu);
        return published.size();
    }
};

// 等待谓词成立(轮询,最长 5s)。
template <typename Pred>
bool wait_until(Pred pred, std::chrono::milliseconds limit = 5000ms) {
    const auto deadline = std::chrono::steady_clock::now() + limit;
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) return true;
        std::this_thread::sleep_for(20ms);
    }
    return pred();
}

} // namespace

// 场景: on_spawned 登记任务 → publish 快照含该任务;子会话 turn 结束
// (BusyChanged false)→ 任务从快照移除(右侧列「只显示运行中」)。
TEST(SubagentHost, PublishesRunningTaskAndRemovesOnIdle) {
    HostFixture fx;
    acecode::SessionOptions opts;
    opts.cwd = fx.cwd.string();
    opts.parent_session_id = fx.parent_id;
    const std::string child_id = fx.host->registry().create(opts);
    ASSERT_FALSE(child_id.empty());

    fx.host->on_spawned(child_id, "do the work");
    EXPECT_EQ(fx.published_count(), 1u);
    {
        std::lock_guard<std::mutex> lk(fx.mu);
        EXPECT_EQ(fx.published[0].id, child_id);
        EXPECT_EQ(fx.published[0].prompt, "do the work");
    }

    // 注入消息驱动一轮 turn;EchoStreamProvider 立即完成 → busy false 事件。
    ASSERT_TRUE(fx.host->client().send_input(child_id, "hello"));
    EXPECT_TRUE(wait_until([&] { return fx.published_count() == 0; }))
        << "turn 结束后任务应从运行中快照移除";
    fx.host->registry().destroy(child_id);
}

// 场景: /tasks list 合并「运行中(registry)+ 已结束(磁盘 parent 匹配)」;
// clear_settled 只删已结束的,不碰运行中。
TEST(SubagentHost, ListMergesAndClearOnlyRemovesSettled) {
    HostFixture fx;
    const auto project_dir = acecode::SessionStorage::get_project_dir(fx.cwd.string());
    fs::create_directories(project_dir);

    // 手造一个已结束的子任务(磁盘 meta + jsonl)。
    const std::string settled_id = "20260705-000100-bbbb";
    {
        acecode::SessionMeta meta;
        meta.id = settled_id;
        meta.cwd = fx.cwd.string();
        meta.parent_session_id = fx.parent_id;
        meta.title = "settled task";
        acecode::SessionStorage::write_meta(
            acecode::SessionStorage::meta_path(project_dir, settled_id), meta);
        std::ofstream(acecode::SessionStorage::session_path(project_dir, settled_id)) << "";
    }
    // 一个运行中的(只登记,不跑 turn → 一直算运行中)。
    acecode::SessionOptions opts;
    opts.cwd = fx.cwd.string();
    opts.parent_session_id = fx.parent_id;
    const std::string running_id = fx.host->registry().create(opts);
    fx.host->on_spawned(running_id, "still running");

    auto entries = fx.host->list_tasks(project_dir);
    ASSERT_EQ(entries.size(), 2u);
    EXPECT_TRUE(entries[0].running);
    EXPECT_EQ(entries[0].id, running_id);
    EXPECT_FALSE(entries[1].running);
    EXPECT_EQ(entries[1].id, settled_id);

    // clear 只删已结束;运行中任务与它的持久化数据不受影响。
    EXPECT_EQ(fx.host->clear_settled(project_dir), 1);
    EXPECT_FALSE(fs::exists(
        acecode::SessionStorage::meta_path(project_dir, settled_id)));
    EXPECT_NE(fx.host->registry().acquire(running_id), nullptr);
    fx.host->registry().destroy(running_id);
}

// 场景: abort_task 对未知 id 返回 false;对运行中任务发 abort 请求成功。
TEST(SubagentHost, AbortTaskRoutesToRegistry) {
    HostFixture fx;
    EXPECT_FALSE(fx.host->abort_task("nope"));
    acecode::SessionOptions opts;
    opts.cwd = fx.cwd.string();
    const std::string id = fx.host->registry().create(opts);
    EXPECT_TRUE(fx.host->abort_task(id));
    fx.host->registry().destroy(id);
}
