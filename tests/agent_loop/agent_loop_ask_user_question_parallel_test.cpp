// 验证 AgentLoop 在并行 read-only batch 路径下,把 AskUserQuestionPrompter
// 包成 ToolContext::ask_user_questions 回调正确注入到每个并行任务。
//
// 一旦回归(并行路径只填 cwd / abort_flag,漏掉 ask_user_questions):
//   - daemon 工厂版 AskUserQuestion 工具立刻 fail-fast 返回
//     "[Error] AskUserQuestion is not supported by this session
//     (no UI channel connected)." —— Web UI / Desktop 的 QuestionModal
//     永远弹不出来。
//
// 测试模式:
//   - 用 StubLlmProvider 脚本化两轮 LLM 输出
//   - 注册真实的 create_ask_user_question_tool_async() 工厂版工具
//   - 用真实的 AskUserQuestionPrompter,绑到 AgentLoop 的 events()
//   - 用 EventDispatcher::subscribe 假扮前端: 收到 QuestionRequest →
//     立刻 prompter.notify_response 模拟用户答题
//   - 断言工具最终 success=true、output 不以 [Error] 开头
//
// 三个子场景:
//   1. ParallelAskUserQuestionGetsCallback —— 装了 prompter 后走通完整问答
//   2. NoPrompterStillFailsFast —— 没装 prompter 时仍然 fail-fast(锁住语义)
//   3. ParallelCtxHasTrackFileWriteBefore —— sanity: 当 set_session_manager
//      非空时,并行 ctx 也注入 track_file_write_before(顺手补的字段一致性)

#include <gtest/gtest.h>

#include "agent_loop.hpp"
#include "stub_provider.hpp"
#include "tool/ask_user_question_tool.hpp"
#include "tool/task_complete_tool.hpp"
#include "tool/tool_executor.hpp"
#include "permissions.hpp"
#include "provider/llm_provider.hpp"
#include "session/ask_user_question_prompter.hpp"
#include "session/event_dispatcher.hpp"
#include "session/session_manager.hpp"

#include <nlohmann/json.hpp>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

using acecode::AgentLoop;
using acecode::AgentCallbacks;
using acecode::AskUserQuestionAnswer;
using acecode::AskUserQuestionPrompter;
using acecode::AskUserQuestionResponse;
using acecode::ChatMessage;
using acecode::EventDispatcher;
using acecode::PermissionManager;
using acecode::PermissionResult;
using acecode::SessionEvent;
using acecode::SessionEventKind;
using acecode::SessionManager;
using acecode::ToolContext;
using acecode::ToolDef;
using acecode::ToolExecutor;
using acecode::ToolImpl;
using acecode::ToolResult;
using acecode::ToolSource;
using acecode_test::ScriptedResponse;
using acecode_test::StubLlmProvider;

using namespace std::chrono_literals;

namespace {

// 构造一个合法的 AskUserQuestion JSON 入参,单题单选,2 个选项。
std::string make_ask_args() {
    nlohmann::json args = {
        {"questions", nlohmann::json::array({
            {
                {"question", "Which library should we use?"},
                {"header",   "Library"},
                {"options",  nlohmann::json::array({
                    {{"label", "axios"},   {"description", "popular http client"}},
                    {{"label", "fetch"},   {"description", "native browser api"}},
                })},
                {"multiSelect", false},
            }
        })}
    };
    return args.dump();
}

// 探针工具: 每次执行把 ToolContext 的关键字段非空状态写到外部 atomic
// (供 ParallelCtxHasTrackFileWriteBefore 断言)。is_read_only=true 让它走
// 并行路径,与 AskUserQuestion 同 batch。
struct CtxProbe {
    std::atomic<bool> seen_track_file_write_before{false};
    std::atomic<bool> seen_ask_user_questions{false};
    std::atomic<int>  call_count{0};
};

ToolImpl make_ctx_probe_tool(CtxProbe* probe) {
    ToolDef def;
    def.name = "ctx_probe";
    def.description = "Probes ToolContext field presence; used by parallel-batch tests.";
    def.parameters = {
        {"type", "object"},
        {"properties", nlohmann::json::object()}
    };
    ToolImpl impl;
    impl.definition = def;
    impl.execute = [probe](const std::string&, const ToolContext& ctx) -> ToolResult {
        if (probe) {
            if (ctx.track_file_write_before) {
                probe->seen_track_file_write_before.store(true);
            }
            if (ctx.ask_user_questions) {
                probe->seen_ask_user_questions.store(true);
            }
            probe->call_count.fetch_add(1);
        }
        return ToolResult{"probed", true};
    };
    impl.is_read_only = true;
    impl.source = ToolSource::Builtin;
    return impl;
}

// Fixture: 包 AgentLoop + 真实 prompter + 真实 EventDispatcher 监听 + 同步原语。
// dtor 取消订阅;AgentLoop 析构 join worker。
class AskParallelHarness {
public:
    explicit AskParallelHarness(bool with_prompter = true,
                                  bool with_session_manager = false) {
        tools_.register_tool(acecode::create_task_complete_tool());
        tools_.register_tool(acecode::create_ask_user_question_tool_async());
        tools_.register_tool(make_ctx_probe_tool(&probe_));

        AgentCallbacks cb;
        cb.on_tool_result = [this](const ChatMessage& /*call*/,
                                      const std::string& tool_name,
                                      const ToolResult& result) {
            std::lock_guard<std::mutex> lk(msg_mu_);
            tool_results_.push_back({tool_name, result.output, result.success});
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

        if (with_session_manager) {
            // SessionManager 不调 start_session — 仅占位,让 set_session_manager
            // 注入非空指针,使并行 batch 注入 track_file_write_before。
            session_manager_ = std::make_unique<SessionManager>();
            loop_->set_session_manager(session_manager_.get());
        }

        if (with_prompter) {
            prompter_ = std::make_unique<AskUserQuestionPrompter>(loop_->events(), 5s);
            // "前端" listener: 看到 QuestionRequest 立即用 canned 答案
            // notify_response。canned = 用户单选了 axios。
            sub_ = loop_->events().subscribe([this](const SessionEvent& e) {
                if (e.kind != SessionEventKind::QuestionRequest) return;
                AskUserQuestionResponse resp;
                resp.cancelled = false;
                AskUserQuestionAnswer ans;
                auto qs = e.payload.value("questions", nlohmann::json::array());
                if (!qs.empty() && qs[0].is_object()) {
                    ans.question_id = qs[0].value("id",
                        std::string{"Which library should we use?"});
                } else {
                    ans.question_id = "Which library should we use?";
                }
                ans.selected = {"axios"};
                resp.answers.push_back(ans);
                std::string rid = e.payload.value("request_id", std::string{});
                {
                    std::lock_guard<std::mutex> lk(req_mu_);
                    last_request_id_ = rid;
                    request_count_++;
                }
                if (prompter_) prompter_->notify_response(rid, resp);
            });
            loop_->set_ask_question_prompter(prompter_.get());
        }
    }

    ~AskParallelHarness() {
        if (sub_ != 0) loop_->events().unsubscribe(sub_);
    }

    void push_text(std::string s) { provider_->push_text(std::move(s)); }
    void push_tool_call(std::string name, std::string args, std::string id = "c1") {
        provider_->push_tool_call(std::move(name), std::move(args), std::move(id));
    }
    void push_task_complete(std::string summary, std::string id = "c-done") {
        nlohmann::json args = {{"summary", std::move(summary)}};
        provider_->push_tool_call("task_complete", args.dump(), std::move(id));
    }
    void push_response(ScriptedResponse r) {
        provider_->push_response(std::move(r));
    }

    bool submit_and_wait(const std::string& msg,
                          std::chrono::milliseconds timeout = std::chrono::seconds(5)) {
        {
            std::lock_guard<std::mutex> lk(busy_mu_);
            is_busy_ = true;
        }
        loop_->submit(msg);
        std::unique_lock<std::mutex> lk(busy_mu_);
        return busy_cv_.wait_for(lk, timeout, [this] { return !is_busy_; });
    }

    struct ToolCallResult {
        std::string tool_name;
        std::string output;
        bool        success = false;
    };

    std::vector<ToolCallResult> snapshot_tool_results() {
        std::lock_guard<std::mutex> lk(msg_mu_);
        return tool_results_;
    }

    int request_count() {
        std::lock_guard<std::mutex> lk(req_mu_);
        return request_count_;
    }

    CtxProbe& probe() { return probe_; }

private:
    std::shared_ptr<StubLlmProvider> provider_ = std::make_shared<StubLlmProvider>();
    ToolExecutor                              tools_;
    PermissionManager                         perms_;
    std::unique_ptr<SessionManager>           session_manager_;
    std::unique_ptr<AskUserQuestionPrompter>  prompter_;
    EventDispatcher::SubscriptionId           sub_ = 0;
    std::unique_ptr<AgentLoop>                loop_;

    CtxProbe                                  probe_;

    std::mutex                  msg_mu_;
    std::vector<ToolCallResult> tool_results_;

    std::mutex                  req_mu_;
    int                         request_count_ = 0;
    std::string                 last_request_id_;

    std::mutex                  busy_mu_;
    std::condition_variable     busy_cv_;
    bool                        is_busy_ = false;
};

} // namespace

// 主场景: 装了 prompter,AskUserQuestion 在并行 batch 中得到 ask_user_questions
// 回调,完成完整问答 → 工具 success=true、output 含 "User has answered..."。
// 这是修 bug 之前直接挂的场景: 并行 ctx 没塞回调 → 工具立刻 fail-fast。
TEST(AgentLoopAskUserQuestionParallel, ParallelAskUserQuestionGetsCallback) {
    AskParallelHarness h(/*with_prompter=*/true);
    h.push_tool_call("AskUserQuestion", make_ask_args(), "ask-1");
    h.push_task_complete("done");

    ASSERT_TRUE(h.submit_and_wait("please ask me about library choice"));
    EXPECT_EQ(h.request_count(), 1) << "前端 listener 应收到 1 次 QuestionRequest";

    auto results = h.snapshot_tool_results();
    bool found_ask = false;
    for (const auto& r : results) {
        if (r.tool_name == "AskUserQuestion") {
            found_ask = true;
            EXPECT_TRUE(r.success) << "AskUserQuestion 应成功(output=" << r.output << ")";
            EXPECT_NE(r.output.find("User has answered your questions"), std::string::npos)
                << "output 应来自 format_ask_answers,实际=" << r.output;
            // 显式锁住: 不能含 "no UI channel connected" 这条 fail-fast 文案
            EXPECT_EQ(r.output.find("no UI channel connected"), std::string::npos)
                << "回归: 并行路径漏注入 ask_user_questions 又出现了。output=" << r.output;
        }
    }
    EXPECT_TRUE(found_ask) << "测试期望至少 1 次 AskUserQuestion tool_result";
}

// 防回归子场景: 没装 prompter 时,工具仍然 fail-fast(锁住保留语义,
// 不让"修了并行路径"反过来掩盖 daemon 配置缺失)。
TEST(AgentLoopAskUserQuestionParallel, NoPrompterStillFailsFast) {
    AskParallelHarness h(/*with_prompter=*/false);
    h.push_tool_call("AskUserQuestion", make_ask_args(), "ask-1");
    h.push_task_complete("done");

    ASSERT_TRUE(h.submit_and_wait("please ask"));

    auto results = h.snapshot_tool_results();
    bool found = false;
    for (const auto& r : results) {
        if (r.tool_name == "AskUserQuestion") {
            found = true;
            EXPECT_FALSE(r.success);
            EXPECT_NE(r.output.find("[Error]"), std::string::npos);
            EXPECT_NE(r.output.find("no UI channel connected"), std::string::npos)
                << "没装 prompter 时仍应保留 fail-fast 文案";
        }
    }
    EXPECT_TRUE(found);
}

// Sanity 子场景: track_file_write_before 在并行 ctx 里也被注入。
// 让 ctx_probe 与 AskUserQuestion 同一 turn 里被 LLM 一起返回 → 走同一并行
// batch → 复用并行 ctx 构造段。Probe 工具记录 ctx.track_file_write_before
// 是否非空。要求 set_session_manager(非空) 才会触发 if 分支。
TEST(AgentLoopAskUserQuestionParallel, ParallelCtxHasTrackFileWriteBefore) {
    AskParallelHarness h(/*with_prompter=*/true, /*with_session_manager=*/true);

    // 单 turn 同时返回两个 read-only tool_calls,触发并行 batch 路径。
    ScriptedResponse r;
    {
        acecode::ToolCall a;
        a.id = "probe-1"; a.function_name = "ctx_probe"; a.function_arguments = "{}";
        r.tool_calls.push_back(std::move(a));
    }
    {
        acecode::ToolCall b;
        b.id = "ask-1"; b.function_name = "AskUserQuestion";
        b.function_arguments = make_ask_args();
        r.tool_calls.push_back(std::move(b));
    }
    h.push_response(std::move(r));
    h.push_task_complete("done");

    ASSERT_TRUE(h.submit_and_wait("probe + ask in one turn"));
    EXPECT_GE(h.probe().call_count.load(), 1) << "ctx_probe 应被执行至少 1 次";
    EXPECT_TRUE(h.probe().seen_track_file_write_before.load())
        << "并行 ctx 应注入 track_file_write_before(session_manager_ 非空)";
    EXPECT_TRUE(h.probe().seen_ask_user_questions.load())
        << "并行 ctx 应注入 ask_user_questions(prompter 非空)";
}
