// 覆盖 src/tool/spawn_subagent_tool.cpp。
//
// spawn_subagent / wait_subagent 是 daemon 的子代理工具:在 SessionRegistry
// 里创建独立子会话并注入首条消息,父会话上下文只吃回最终摘要。一旦回归:
//   - 深度限制失效 → 子代理递归派生,会话数失控
//   - wait 判定失效 → 父会话死等 / 提前返回空结果
//   - deps 未回填时崩溃(而不是报"仅 daemon 可用")
//
// 测试不真跑 LLM:EchoStreamProvider 的 chat_stream 立即产出固定文本,
// 让子会话 turn 有真实的 assistant 消息可供 wait 逻辑取回。

#include <gtest/gtest.h>

#include "config/config.hpp"
#include "experts/expert_registry.hpp"
#include "permissions.hpp"
#include "session/local_session_client.hpp"
#include "session/session_registry.hpp"
#include "session/session_storage.hpp"
#include "tool/spawn_subagent_tool.hpp"
#include "tool/tool_executor.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <random>

using namespace std::chrono_literals;
namespace fs = std::filesystem;

namespace {

void write_test_skill(const fs::path& workspace,
                      const std::string& name,
                      const std::string& description) {
    const fs::path dir = workspace / ".acecode" / "skills" / name;
    fs::create_directories(dir);
    std::ofstream out(dir / "SKILL.md", std::ios::binary);
    out << "---\n"
        << "name: " << name << "\n"
        << "description: " << description << "\n"
        << "---\n\n"
        << "# " << name << "\n";
}

// 立即回复固定文本的 stub:子会话 turn 会产生一条 assistant 消息。
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

class RecordingSessionClient : public acecode::SessionClient {
public:
    const bool* on_spawn_seen = nullptr;
    bool send_saw_on_spawn = false;
    std::string sent_session_id;
    std::string sent_text;
    std::string sent_display_text;

    std::string create_session(const acecode::SessionOptions&) override { return {}; }
    bool resume_session(const std::string&, const acecode::SessionOptions& = {}) override {
        return false;
    }
    std::vector<acecode::SessionInfo> list_sessions() override { return {}; }
    void destroy_session(const std::string&) override {}
    SubscriptionId subscribe(const std::string&,
                             EventListener,
                             std::uint64_t = 0) override {
        return 0;
    }
    void unsubscribe(const std::string&, SubscriptionId) override {}
    bool send_input(const std::string& session_id,
                    const std::string& text) override {
        return send_input(session_id, text, std::string{});
    }
    bool send_input(const std::string& session_id,
                    const std::string& text,
                    const std::string& display_text) override {
        send_saw_on_spawn = on_spawn_seen && *on_spawn_seen;
        sent_session_id = session_id;
        sent_text = text;
        sent_display_text = display_text;
        return true;
    }
    acecode::BuiltinCommandResult execute_builtin_command(
        const std::string&,
        const acecode::BuiltinCommandRequest&) override {
        return {};
    }
    void respond_permission(const std::string&,
                            const acecode::PermissionDecision&) override {}
    acecode::QuestionResponseStatus respond_question(
        const std::string&,
        const std::string&,
        const acecode::AskUserQuestionResponse&) override {
        return acecode::QuestionResponseStatus::Closed;
    }
    void abort(const std::string&) override {}
};

struct SubagentFixture {
    acecode::ToolExecutor tools;
    acecode::PermissionManager permissions;
    acecode::AppConfig config;
    std::shared_ptr<acecode::LlmProvider> provider;
    fs::path cwd;
    acecode::ExpertRegistry experts;
    acecode::SessionRegistry registry;
    acecode::LocalSessionClient client;
    std::shared_ptr<acecode::SubagentToolDeps> deps;

    SubagentFixture()
        : cwd(fs::temp_directory_path() /
              ("acecode_subagent_test_" + std::to_string(std::random_device{}()))),
          experts(cwd / "global-experts"),
          registry(make_deps(*this)), client(registry) {
        fs::create_directories(cwd);
        deps = std::make_shared<acecode::SubagentToolDeps>();
        deps->registry = &registry;
        deps->client = &client;
        deps->config = &config;
        tools.register_tool(acecode::create_spawn_subagent_tool(deps));
        tools.register_tool(acecode::create_wait_subagent_tool(deps));
    }

    ~SubagentFixture() {
        std::error_code ec;
        fs::remove_all(cwd, ec);
    }

    static acecode::SessionRegistryDeps make_deps(SubagentFixture& self) {
        acecode::SessionRegistryDeps d;
        d.provider_accessor = [&self] { return self.provider; };
        d.tools = &self.tools;
        d.cwd = "/tmp/subagent_test_registry";
        d.expert_registry = &self.experts;
        d.template_permissions = &self.permissions;
        return d;
    }

    acecode::ToolContext ctx_for(const std::string& session_id) {
        acecode::ToolContext ctx;
        ctx.cwd = cwd.string();
        if (!session_id.empty()) {
            if (auto entry = registry.acquire(session_id)) {
                ctx.session_manager = entry->sm.get();
            }
        }
        return ctx;
    }
};

} // namespace

// 场景: deps 未回填(TUI / registry 尚未构造)→ 报"仅 daemon 可用"而不是
// 解引用空指针崩溃。
TEST(SpawnSubagentTool, RejectsWhenDepsMissing) {
    auto empty = std::make_shared<acecode::SubagentToolDeps>();
    auto tool = acecode::create_spawn_subagent_tool(empty);
    auto r = tool.execute(R"({"prompt":"hi"})", acecode::ToolContext{});
    EXPECT_FALSE(r.success);
    EXPECT_NE(r.output.find("daemon"), std::string::npos);
}

// 场景: prompt 缺失 → 参数错误,不创建任何会话。
TEST(SpawnSubagentTool, RejectsEmptyPrompt) {
    SubagentFixture fx;
    auto r = fx.tools.execute("spawn_subagent", R"({})", fx.ctx_for(""));
    EXPECT_FALSE(r.success);
    EXPECT_EQ(fx.registry.size(), 0u);
}

// 场景: wait=false 点火即返 —— 返回 subagent_session_id,registry 里能找到
// 该会话且 subagent_depth=1(供后续深度限制判定)。这是流水线接力的形态。
TEST(SpawnSubagentTool, FireAndForgetCreatesIsolatedSession) {
    SubagentFixture fx;
    fx.provider = std::make_shared<EchoStreamProvider>();

    auto r = fx.tools.execute("spawn_subagent",
                              R"({"prompt":"stage two go","wait":false})",
                              fx.ctx_for(""));
    ASSERT_TRUE(r.success) << r.output;
    ASSERT_TRUE(r.metadata.contains("subagent_session_id"));
    const std::string child_id = r.metadata["subagent_session_id"].get<std::string>();
    ASSERT_FALSE(child_id.empty());

    auto child = fx.registry.acquire(child_id);
    ASSERT_NE(child, nullptr);
    EXPECT_EQ(child->subagent_depth, 1);
    fx.registry.destroy(child_id);
}

// 场景: daemon 要在子会话首条输入入队前安装 tracking 监听器,否则 child
// worker 可能抢先 emit busy=true / permission_request,主会话 UI 仍然发现不了
// 这个后台任务。用 fake SessionClient 确定性验证 on_spawn 早于 send_input。
TEST(SpawnSubagentTool, CallsOnSpawnBeforeSendingFirstInput) {
    SubagentFixture fx;
    RecordingSessionClient recording_client;
    bool on_spawn_seen = false;
    std::string spawned_child_id;
    recording_client.on_spawn_seen = &on_spawn_seen;
    fx.deps->client = &recording_client;
    fx.deps->on_spawn = [&](const std::string& child_id,
                            const std::string& /*prompt*/) {
        spawned_child_id = child_id;
        on_spawn_seen = true;
    };

    auto r = fx.tools.execute("spawn_subagent",
                              R"({"prompt":"stage two go","wait":false})",
                              fx.ctx_for(""));
    ASSERT_TRUE(r.success) << r.output;
    ASSERT_TRUE(r.metadata.contains("subagent_session_id"));
    const std::string child_id =
        r.metadata["subagent_session_id"].get<std::string>();

    EXPECT_EQ(spawned_child_id, child_id);
    EXPECT_EQ(recording_client.sent_session_id, child_id);
    EXPECT_TRUE(recording_client.send_saw_on_spawn)
        << "tracking must be installed before the first child input is queued";
    fx.registry.destroy(child_id);
}

TEST(SpawnSubagentTool, SkillExpansionUsesInvocationAllowlist) {
    SubagentFixture fx;
    const std::string allowed = "headless-allowed-skill";
    const std::string blocked = "headless-blocked-skill";
    write_test_skill(fx.cwd, allowed, "selected by this invocation");
    write_test_skill(fx.cwd, blocked, "must remain hidden");
    fx.config.skills.allowed = std::vector<std::string>{allowed};

    RecordingSessionClient recording_client;
    fx.deps->client = &recording_client;

    const std::string allowed_prompt = "/" + allowed + " inspect this";
    auto expanded = fx.tools.execute(
        "spawn_subagent",
        std::string(R"({"prompt":")") + allowed_prompt + R"(","wait":false})",
        fx.ctx_for(""));
    ASSERT_TRUE(expanded.success) << expanded.output;
    EXPECT_EQ(recording_client.sent_display_text, allowed_prompt);
    EXPECT_NE(recording_client.sent_text, allowed_prompt);
    EXPECT_NE(recording_client.sent_text.find(allowed), std::string::npos);
    const std::string first_child =
        expanded.metadata["subagent_session_id"].get<std::string>();
    fx.registry.destroy(first_child);

    const std::string blocked_prompt = "/" + blocked + " inspect this";
    auto untouched = fx.tools.execute(
        "spawn_subagent",
        std::string(R"({"prompt":")") + blocked_prompt + R"(","wait":false})",
        fx.ctx_for(""));
    ASSERT_TRUE(untouched.success) << untouched.output;
    EXPECT_EQ(recording_client.sent_text, blocked_prompt);
    EXPECT_TRUE(recording_client.sent_display_text.empty());
    const std::string second_child =
        untouched.metadata["subagent_session_id"].get<std::string>();
    fx.registry.destroy(second_child);
}

// 场景: 子代理不能再派生子代理 —— 用子会话自己的 SessionManager 作为调用
// 上下文再次 spawn,必须被拒绝且不产生新会话。回归表现:递归派生失控。
TEST(SpawnSubagentTool, SubagentCannotSpawnFurther) {
    SubagentFixture fx;
    fx.provider = std::make_shared<EchoStreamProvider>();

    auto first = fx.tools.execute("spawn_subagent",
                                  R"({"prompt":"child","wait":false})",
                                  fx.ctx_for(""));
    ASSERT_TRUE(first.success) << first.output;
    const std::string child_id =
        first.metadata["subagent_session_id"].get<std::string>();
    const std::size_t before = fx.registry.size();

    auto nested = fx.tools.execute("spawn_subagent",
                                   R"({"prompt":"grandchild","wait":false})",
                                   fx.ctx_for(child_id));
    EXPECT_FALSE(nested.success);
    EXPECT_NE(nested.output.find("cannot spawn"), std::string::npos);
    EXPECT_EQ(fx.registry.size(), before) << "被拒绝时不应创建新会话";
    fx.registry.destroy(child_id);
}

TEST(SpawnSubagentTool, TeamLeadCanSpawnOnlyDeclaredExpertMember) {
    SubagentFixture fx;
    acecode::ExpertDraft coordinator;
    coordinator.id = "coordinator";
    coordinator.display_name = "Coordinator";
    coordinator.profession = "Delivery";
    coordinator.lead = {
        "lead", "Coordinator", "Delivery", "Coordinate the work."};
    std::string error;
    ASSERT_TRUE(fx.experts.create_global(coordinator, &error)) << error;

    acecode::ExpertDraft tester;
    tester.id = "tester";
    tester.display_name = "Tester";
    tester.profession = "QA";
    tester.lead = {"lead", "Tester", "QA", "Test the work."};
    ASSERT_TRUE(fx.experts.create_global(tester, &error)) << error;

    acecode::ExpertDraft team;
    team.id = "delivery-team";
    team.type = acecode::ExpertType::Team;
    team.display_name = "Delivery Team";
    team.profession = "Delivery";
    team.lead_expert_id = "coordinator";
    team.member_expert_ids = {"tester"};
    ASSERT_TRUE(fx.experts.create_global(team, &error)) << error;

    acecode::SessionOptions parent_options;
    parent_options.cwd = fx.cwd.string();
    parent_options.expert_id = team.id;
    const std::string parent_id = fx.registry.create(parent_options);

    const std::size_t before = fx.registry.size();
    auto rejected = fx.tools.execute(
        "spawn_subagent",
        R"({"prompt":"attack","wait":false,"expert_member":"intruder"})",
        fx.ctx_for(parent_id));
    EXPECT_FALSE(rejected.success);
    EXPECT_NE(rejected.output.find("not selected"), std::string::npos);
    EXPECT_EQ(fx.registry.size(), before);

    RecordingSessionClient recording_client;
    fx.deps->client = &recording_client;
    auto accepted = fx.tools.execute(
        "spawn_subagent",
        R"({"prompt":"test it","wait":false,"expert_member":"tester"})",
        fx.ctx_for(parent_id));
    ASSERT_TRUE(accepted.success) << accepted.output;
    const std::string child_id =
        accepted.metadata["subagent_session_id"].get<std::string>();
    auto child = fx.registry.acquire(child_id);
    ASSERT_NE(child, nullptr);
    EXPECT_EQ(child->expert_id, team.id);
    EXPECT_EQ(child->expert_member_id, "tester");
    ASSERT_TRUE(child->expert.has_value());
    ASSERT_NE(child->expert->selected_agent("tester"), nullptr);
    EXPECT_EQ(child->expert->selected_agent("tester")->instructions, "Test the work.");

    fx.registry.destroy(child_id);
    fx.registry.destroy(parent_id);
}

TEST(SpawnSubagentTool, OrdinarySessionCannotRequestExpertMember) {
    SubagentFixture fx;
    acecode::SessionOptions parent_options;
    parent_options.cwd = fx.cwd.string();
    const std::string parent_id = fx.registry.create(parent_options);
    const auto result = fx.tools.execute(
        "spawn_subagent",
        R"({"prompt":"test it","wait":false,"expert_member":"tester"})",
        fx.ctx_for(parent_id));
    EXPECT_FALSE(result.success);
    EXPECT_NE(result.output.find("team expert lead"), std::string::npos);
    EXPECT_EQ(fx.registry.size(), 1u);
    fx.registry.destroy(parent_id);
}

// 场景: wait=true(默认)阻塞至子会话本轮结束,并把最终 assistant 答复带回
// 父上下文。EchoStreamProvider 立即完成,wait 逻辑要能在"从未观测到 busy"
// (turn 快于轮询间隔)的情况下靠新消息判定完成,而不是死等。
TEST(SpawnSubagentTool, WaitReturnsFinalAssistantReply) {
    SubagentFixture fx;
    fx.provider = std::make_shared<EchoStreamProvider>();

    auto r = fx.tools.execute("spawn_subagent",
                              R"({"prompt":"do the work","timeout_seconds":30})",
                              fx.ctx_for(""));
    ASSERT_TRUE(r.success) << r.output;
    EXPECT_NE(r.output.find("subagent-final-reply"), std::string::npos)
        << "父上下文应拿到子会话的最终答复,实际: " << r.output;
    ASSERT_TRUE(r.metadata.contains("subagent_session_id"));
    fx.registry.destroy(r.metadata["subagent_session_id"].get<std::string>());
}

// 场景: 有父会话上下文的 spawn → 子会话 entry 与持久化 meta 都记录
// parent_session_id(「后台任务」面板/列表过滤的数据源)。用 wait=true
// 保证子会话 turn 已结束、meta 已落盘。一旦回归:子会话泄漏进侧栏列表。
TEST(SpawnSubagentTool, ChildRecordsParentSessionId) {
    SubagentFixture fx;
    fx.provider = std::make_shared<EchoStreamProvider>();

    acecode::SessionOptions parent_opts;
    parent_opts.cwd = fx.cwd.string();
    const std::string parent_id = fx.registry.create(parent_opts);
    ASSERT_FALSE(parent_id.empty());

    auto r = fx.tools.execute("spawn_subagent",
                              R"({"prompt":"go","timeout_seconds":30})",
                              fx.ctx_for(parent_id));
    ASSERT_TRUE(r.success) << r.output;
    const std::string child_id =
        r.metadata["subagent_session_id"].get<std::string>();

    auto child = fx.registry.acquire(child_id);
    ASSERT_NE(child, nullptr);
    EXPECT_EQ(child->parent_session_id, parent_id);
    ASSERT_NE(child->sm, nullptr);
    auto meta = child->sm->load_session_meta(child_id);
    ASSERT_EQ(meta.id, child_id) << "子会话 meta 应已落盘";
    EXPECT_EQ(meta.parent_session_id, parent_id)
        << "parent_session_id 必须持久化,否则 daemon 重启后子会话泄漏进侧栏";

    fx.registry.destroy(child_id);
    fx.registry.destroy(parent_id);
}

// 场景: daemon 重启(registry 冷启动)后 resume 一个带 parent_session_id
// 的子会话 → 子会话身份(parent_session_id + subagent_depth=1)从 meta 恢复,
// 深度限制继续生效。一旦回归:重启后的子会话能再派生孙代理。
TEST(SpawnSubagentTool, ResumeRestoresSubagentIdentityFromMeta) {
    SubagentFixture fx;
    fx.provider = std::make_shared<EchoStreamProvider>();

    const std::string child_id = "20260705-010203-beef";
    const auto project_dir =
        acecode::SessionStorage::get_project_dir(fx.cwd.string());
    fs::create_directories(project_dir);
    {
        // 手造持久化数据:meta 带 parent_session_id,jsonl 只需存在。
        acecode::SessionMeta meta;
        meta.id = child_id;
        meta.cwd = fx.cwd.string();
        meta.parent_session_id = "20260705-010000-cafe";
        acecode::SessionStorage::write_meta(
            acecode::SessionStorage::meta_path(project_dir, child_id), meta);
        std::ofstream jsonl(
            acecode::SessionStorage::session_path(project_dir, child_id));
        jsonl << "";
    }

    acecode::SessionOptions opts;
    opts.cwd = fx.cwd.string();
    ASSERT_TRUE(fx.registry.resume(child_id, opts));
    auto entry = fx.registry.acquire(child_id);
    ASSERT_NE(entry, nullptr);
    EXPECT_EQ(entry->parent_session_id, "20260705-010000-cafe");
    EXPECT_EQ(entry->subagent_depth, 1)
        << "深度限制必须随 meta 恢复,防重启后递归派生";

    auto nested = fx.tools.execute("spawn_subagent",
                                   R"({"prompt":"grandchild","wait":false})",
                                   fx.ctx_for(child_id));
    EXPECT_FALSE(nested.success);
    fx.registry.destroy(child_id);
}

// 场景: wait_subagent 对不存在的 session id → 明确报错,不阻塞。
TEST(WaitSubagentTool, UnknownSessionRejected) {
    SubagentFixture fx;
    auto r = fx.tools.execute("wait_subagent",
                              R"({"session_id":"nope-123"})",
                              fx.ctx_for(""));
    EXPECT_FALSE(r.success);
    EXPECT_NE(r.output.find("unknown session"), std::string::npos);
}

// 场景: wait_subagent 对已完成的子会话 → 直接取回其最新答复(配合
// spawn(wait=false) 的 fan-out / join 用法)。
TEST(WaitSubagentTool, CollectsReplyFromFinishedSubagent) {
    SubagentFixture fx;
    fx.provider = std::make_shared<EchoStreamProvider>();

    auto spawned = fx.tools.execute("spawn_subagent",
                                    R"({"prompt":"go","wait":false})",
                                    fx.ctx_for(""));
    ASSERT_TRUE(spawned.success) << spawned.output;
    const std::string child_id =
        spawned.metadata["subagent_session_id"].get<std::string>();

    auto r = fx.tools.execute(
        "wait_subagent",
        std::string(R"({"session_id":")") + child_id + R"(","timeout_seconds":30})",
        fx.ctx_for(""));
    ASSERT_TRUE(r.success) << r.output;
    EXPECT_NE(r.output.find("subagent-final-reply"), std::string::npos);
    fx.registry.destroy(child_id);
}
