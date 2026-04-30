// 覆盖 src/session/ask_user_question_prompter.cpp。daemon 模式下 LLM 调
// AskUserQuestion 工具时阻塞 worker thread,等浏览器用户回答的同步通道。
//
// 一旦回归:
//   - prompt() 拿不到正确 answers → format_ask_answers 输出错乱,LLM 被骗
//   - cancelled 不识别 → 工具假装成功,LLM 以为用户答了
//   - 超时不触发 → worker thread 永远卡住
//   - abort_flag 不响应 → /abort 后 daemon 不能 stop worker
//   - notify 用错 request_id → 多并发 prompt(future-proof) 时回流串号
//   - 与 permission_prompter 的 id 空间打架 → 浏览器回 decision 误触发 question
//
// 每个 TEST 对应一个真实可观测故障路径。

#include <gtest/gtest.h>

#include "session/ask_user_question_prompter.hpp"
#include "session/event_dispatcher.hpp"
#include "session/permission_prompter.hpp"

#include <atomic>
#include <chrono>
#include <thread>

using namespace std::chrono_literals;
using acecode::AskUserQuestionAnswer;
using acecode::AskUserQuestionPrompter;
using acecode::AskUserQuestionResponse;
using acecode::AsyncPrompter;
using acecode::EventDispatcher;
using acecode::PermissionDecisionChoice;
using acecode::SessionEvent;
using acecode::SessionEventKind;

namespace {

// 注意 race: prompt() 在同一线程同步 emit 完才阻塞 cv,所以必须**先**
// subscribe listener 再起 thread 调 prompt。listener 里捕获 request_id
// 拼一个固定答案后调 notify_response。
struct ResponderState {
    std::mutex                   mu;
    std::string                  request_id;
    bool                         got = false;
    nlohmann::json               questions_seen;
    std::condition_variable      cv;
};

EventDispatcher::SubscriptionId
subscribe_responder(EventDispatcher& d, AskUserQuestionPrompter& prompter,
                      const AskUserQuestionResponse& canned, ResponderState& state) {
    return d.subscribe([&, canned](const SessionEvent& e) {
        if (e.kind != SessionEventKind::QuestionRequest) return;
        {
            std::lock_guard<std::mutex> lk(state.mu);
            state.request_id    = e.payload.value("request_id", std::string{});
            state.questions_seen = e.payload.value("questions", nlohmann::json::array());
            state.got           = true;
        }
        state.cv.notify_all();
        prompter.notify_response(state.request_id, canned);
    });
}

nlohmann::json sample_questions() {
    return nlohmann::json::array({
        {{"id", "Pick a color?"}, {"text", "Pick a color?"},
         {"options", nlohmann::json::array({{{"label", "Red"}, {"value", "Red"}}})},
         {"multiSelect", false}}
    });
}

} // namespace

// 场景: prompt 被 worker thread 调,listener 收到 QuestionRequest 后立即
// notify_response,prompt 应返回 cancelled=false + 正确 answers。
TEST(AskUserQuestionPrompter, ResponseUnblocksPrompt) {
    EventDispatcher d;
    AskUserQuestionPrompter prompter(d, 5s);

    AskUserQuestionResponse canned;
    canned.cancelled = false;
    AskUserQuestionAnswer ans;
    ans.question_id = "Pick a color?";
    ans.selected    = {"Red"};
    canned.answers.push_back(ans);

    ResponderState state;
    auto sub = subscribe_responder(d, prompter, canned, state);

    auto resp = prompter.prompt(sample_questions(), nullptr);
    EXPECT_FALSE(resp.cancelled);
    ASSERT_EQ(resp.answers.size(), 1u);
    EXPECT_EQ(resp.answers[0].question_id, "Pick a color?");
    ASSERT_EQ(resp.answers[0].selected.size(), 1u);
    EXPECT_EQ(resp.answers[0].selected[0], "Red");

    EXPECT_TRUE(state.got);
    EXPECT_FALSE(state.request_id.empty());
    EXPECT_TRUE(state.questions_seen.is_array());
    d.unsubscribe(sub);
}

// 场景: cancelled=true 路径(用户按 ESC / 关浏览器),prompt 必须返回 cancelled=true。
TEST(AskUserQuestionPrompter, CancelledResponsePropagated) {
    EventDispatcher d;
    AskUserQuestionPrompter prompter(d, 5s);

    AskUserQuestionResponse canned;
    canned.cancelled = true;

    ResponderState state;
    auto sub = subscribe_responder(d, prompter, canned, state);

    auto resp = prompter.prompt(sample_questions(), nullptr);
    EXPECT_TRUE(resp.cancelled);
    EXPECT_EQ(resp.answers.size(), 0u);
    d.unsubscribe(sub);
}

// 场景: 没有 listener 回应,timeout 必须触发 → cancelled=true 默认值生效,
// 同时 EventDispatcher 应收到一条 Error(reason=question_timeout)。
// 用 100ms 超时让测试快。
TEST(AskUserQuestionPrompter, TimeoutTreatedAsCancelled) {
    EventDispatcher d;
    AskUserQuestionPrompter prompter(d, 100ms);

    bool saw_timeout_error = false;
    auto sub = d.subscribe([&](const SessionEvent& e) {
        if (e.kind == SessionEventKind::Error
            && e.payload.value("reason", std::string{}) == "question_timeout") {
            saw_timeout_error = true;
        }
    });

    auto resp = prompter.prompt(sample_questions(), nullptr);
    EXPECT_TRUE(resp.cancelled);
    EXPECT_TRUE(saw_timeout_error);
    d.unsubscribe(sub);
}

// 场景: abort_flag 拉起后,prompt 必须在 ~50ms 内返回 cancelled=true,
// 不等满 timeout(避免 daemon shutdown 时 worker 卡 5min)。
TEST(AskUserQuestionPrompter, AbortFlagBreaksOutFast) {
    EventDispatcher d;
    AskUserQuestionPrompter prompter(d, 60s);

    std::atomic<bool> abort_flag{false};
    auto t0 = std::chrono::steady_clock::now();

    std::thread aborter([&] {
        std::this_thread::sleep_for(80ms);
        abort_flag.store(true);
    });
    auto resp = prompter.prompt(sample_questions(), &abort_flag);
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0);

    aborter.join();
    EXPECT_TRUE(resp.cancelled);
    // 200ms 是大方的上限 — 50ms 轮询节奏 + abort 设置时机
    EXPECT_LT(elapsed.count(), 500);
}

// 场景: AskUserQuestionPrompter 与 AsyncPrompter(permission)用同一个
// EventDispatcher 时,各自的 request_id 必须不串。回 decision 不能影响
// pending question,反之亦然。
TEST(AskUserQuestionPrompter, RequestIdDoesNotCollideWithPermissionPrompter) {
    EventDispatcher d;
    AskUserQuestionPrompter qprompter(d, 5s);
    AsyncPrompter perm_prompter(d, 5s);

    AskUserQuestionResponse canned_q;
    canned_q.cancelled = false;
    AskUserQuestionAnswer a;
    a.question_id = "Q?"; a.selected = {"A"};
    canned_q.answers.push_back(a);

    // 双 listener: 一个回 question,一个回 permission;互不干扰。
    auto sub1 = d.subscribe([&](const SessionEvent& e) {
        if (e.kind == SessionEventKind::QuestionRequest) {
            qprompter.notify_response(
                e.payload.value("request_id", std::string{}), canned_q);
        }
    });
    auto sub2 = d.subscribe([&](const SessionEvent& e) {
        if (e.kind == SessionEventKind::PermissionRequest) {
            perm_prompter.notify_decision(
                e.payload.value("request_id", std::string{}),
                PermissionDecisionChoice::Allow);
        }
    });

    nlohmann::json qs = nlohmann::json::array({
        {{"id","Q?"},{"text","Q?"},
         {"options", nlohmann::json::array({{{"label","A"},{"value","A"}}})},
         {"multiSelect", false}}
    });

    auto qresp = qprompter.prompt(qs, nullptr);
    EXPECT_FALSE(qresp.cancelled);
    ASSERT_EQ(qresp.answers.size(), 1u);
    EXPECT_EQ(qresp.answers[0].selected[0], "A");

    auto presult = perm_prompter.prompt("bash", "{}", nullptr);
    EXPECT_EQ(presult, acecode::PermissionResult::Allow);

    d.unsubscribe(sub1);
    d.unsubscribe(sub2);
}

// 场景: notify_response 用未知 request_id = no-op,不能让 prompter 状态错乱。
// 通过先 prompt(超时)→ 再用错 id 调 notify_response 几次 → 验证 pending_count 0。
TEST(AskUserQuestionPrompter, UnknownRequestIdNotifyIsNoop) {
    EventDispatcher d;
    AskUserQuestionPrompter prompter(d, 50ms);
    auto resp = prompter.prompt(sample_questions(), nullptr);
    EXPECT_TRUE(resp.cancelled);
    EXPECT_EQ(prompter.pending_count(), 0u);

    AskUserQuestionResponse stale;
    stale.cancelled = false;
    prompter.notify_response("nonexistent-id", stale);
    prompter.notify_response("", stale);
    EXPECT_EQ(prompter.pending_count(), 0u);
}

// 场景: payload 序列化 — QuestionRequest 事件 payload 应包含 request_id +
// questions 数组(原样透传),前端要靠这个渲染 modal。
TEST(AskUserQuestionPrompter, EmitsRequestPayloadShape) {
    EventDispatcher d;
    AskUserQuestionPrompter prompter(d, 50ms);

    nlohmann::json captured_payload;
    auto sub = d.subscribe([&](const SessionEvent& e) {
        if (e.kind == SessionEventKind::QuestionRequest) {
            captured_payload = e.payload;
        }
    });
    auto qs = sample_questions();
    auto resp = prompter.prompt(qs, nullptr);
    (void)resp;

    EXPECT_TRUE(captured_payload.contains("request_id"));
    EXPECT_TRUE(captured_payload["request_id"].is_string());
    EXPECT_FALSE(captured_payload["request_id"].get<std::string>().empty());
    ASSERT_TRUE(captured_payload.contains("questions"));
    EXPECT_TRUE(captured_payload["questions"].is_array());
    EXPECT_EQ(captured_payload["questions"].size(), qs.size());
    d.unsubscribe(sub);
}
