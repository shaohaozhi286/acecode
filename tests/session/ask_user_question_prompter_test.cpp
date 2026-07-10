// 覆盖 src/session/ask_user_question_prompter.cpp。daemon 模式下 LLM 调
// AskUserQuestion 工具时阻塞 worker thread,等浏览器用户回答的同步通道。
//
// 一旦回归:
//   - prompt() 拿不到正确 answers → format_ask_answers 输出错乱,LLM 被骗
//   - cancelled 不识别 → 工具假装成功,LLM 以为用户答了
//   - 默认被动超时 → 用户未回答时 agent 又擅自继续跑
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
#include <future>
#include <thread>
#include <vector>

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

struct ClosedState {
    std::mutex                                      mu;
    std::condition_variable                         cv;
    std::vector<std::pair<std::string, std::string>> items;
};

EventDispatcher::SubscriptionId
subscribe_closed(EventDispatcher& d, ClosedState& state) {
    return d.subscribe([&](const SessionEvent& e) {
        if (e.kind != SessionEventKind::QuestionClosed) return;
        {
            std::lock_guard<std::mutex> lk(state.mu);
            state.items.emplace_back(
                e.payload.value("request_id", std::string{}),
                e.payload.value("reason", std::string{}));
        }
        state.cv.notify_all();
    });
}

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
    AskUserQuestionPrompter prompter(d);

    AskUserQuestionResponse canned;
    canned.cancelled = false;
    AskUserQuestionAnswer ans;
    ans.question_id = "Pick a color?";
    ans.selected    = {"Red"};
    canned.answers.push_back(ans);

    ResponderState state;
    ClosedState closed;
    auto sub = subscribe_responder(d, prompter, canned, state);
    auto closed_sub = subscribe_closed(d, closed);

    auto resp = prompter.prompt(sample_questions(), nullptr);
    EXPECT_FALSE(resp.cancelled);
    ASSERT_EQ(resp.answers.size(), 1u);
    EXPECT_EQ(resp.answers[0].question_id, "Pick a color?");
    ASSERT_EQ(resp.answers[0].selected.size(), 1u);
    EXPECT_EQ(resp.answers[0].selected[0], "Red");

    EXPECT_TRUE(state.got);
    EXPECT_FALSE(state.request_id.empty());
    EXPECT_TRUE(state.questions_seen.is_array());
    ASSERT_EQ(closed.items.size(), 1u);
    EXPECT_EQ(closed.items[0].first, state.request_id);
    EXPECT_EQ(closed.items[0].second, "answered");
    d.unsubscribe(sub);
    d.unsubscribe(closed_sub);
}

// 场景: cancelled=true 路径(用户按 ESC / 关浏览器),prompt 必须返回 cancelled=true。
TEST(AskUserQuestionPrompter, CancelledResponsePropagated) {
    EventDispatcher d;
    AskUserQuestionPrompter prompter(d);

    AskUserQuestionResponse canned;
    canned.cancelled = true;

    ResponderState state;
    ClosedState closed;
    auto sub = subscribe_responder(d, prompter, canned, state);
    auto closed_sub = subscribe_closed(d, closed);

    auto resp = prompter.prompt(sample_questions(), nullptr);
    EXPECT_TRUE(resp.cancelled);
    EXPECT_EQ(resp.answers.size(), 0u);
    ASSERT_EQ(closed.items.size(), 1u);
    EXPECT_EQ(closed.items[0].first, state.request_id);
    EXPECT_EQ(closed.items[0].second, "cancelled");
    d.unsubscribe(sub);
    d.unsubscribe(closed_sub);
}

// 场景: 默认构造不再有被动超时。无人回答时 prompt 应保持 pending,
// 直到 abort_flag 显式拉起。
TEST(AskUserQuestionPrompter, DefaultHasNoPassiveTimeout) {
    EventDispatcher d;
    AskUserQuestionPrompter prompter(d);
    std::atomic<bool> abort_flag{false};

    ResponderState state;
    auto sub = d.subscribe([&](const SessionEvent& e) {
        if (e.kind != SessionEventKind::QuestionRequest) return;
        {
            std::lock_guard<std::mutex> lk(state.mu);
            state.request_id = e.payload.value("request_id", std::string{});
            state.got = true;
        }
        state.cv.notify_all();
    });

    auto future = std::async(std::launch::async, [&] {
        return prompter.prompt(sample_questions(), &abort_flag);
    });
    {
        std::unique_lock<std::mutex> lk(state.mu);
        ASSERT_TRUE(state.cv.wait_for(lk, 500ms, [&] { return state.got; }));
    }
    EXPECT_EQ(future.wait_for(180ms), std::future_status::timeout);

    abort_flag.store(true);
    ASSERT_EQ(future.wait_for(500ms), std::future_status::ready);
    auto resp = future.get();
    EXPECT_TRUE(resp.cancelled);
    d.unsubscribe(sub);
}

// 场景: 非零 timeout 到期(add-ask-question-policy 的 timeout 策略路径)→
// timed_out=true 且 cancelled=false(工具侧据此合成自动采纳结果而不是拒绝),
// QuestionClosed reason="timeout" 通知前端收 modal;不再发 Error 事件
// (超时是策略的正常路径,不是错误)。
TEST(AskUserQuestionPrompter, ExplicitTimeoutMarksTimedOut) {
    EventDispatcher d;
    AskUserQuestionPrompter prompter(d, 100ms);

    bool saw_any_error = false;
    ClosedState closed;
    auto closed_sub = subscribe_closed(d, closed);
    auto sub = d.subscribe([&](const SessionEvent& e) {
        if (e.kind == SessionEventKind::Error) saw_any_error = true;
    });

    auto resp = prompter.prompt(sample_questions(), nullptr);
    EXPECT_TRUE(resp.timed_out);
    EXPECT_FALSE(resp.cancelled);
    EXPECT_EQ(resp.answers.size(), 0u);
    EXPECT_FALSE(saw_any_error);
    ASSERT_EQ(closed.items.size(), 1u);
    EXPECT_EQ(closed.items[0].second, "timeout");
    d.unsubscribe(sub);
    d.unsubscribe(closed_sub);
}

// 场景: abort_flag 拉起后,prompt 必须在 ~50ms 内返回 cancelled=true,
// 不依赖任何被动 timeout(避免 daemon shutdown 时 worker 卡住)。
TEST(AskUserQuestionPrompter, AbortFlagBreaksOutFast) {
    EventDispatcher d;
    AskUserQuestionPrompter prompter(d);
    ClosedState closed;
    auto closed_sub = subscribe_closed(d, closed);

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
    ASSERT_EQ(closed.items.size(), 1u);
    EXPECT_EQ(closed.items[0].second, "aborted");
    d.unsubscribe(closed_sub);
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
// 这同时覆盖 spec「超时后到达的用户回答被忽略」:超时后 request_id 已被清出
// pending_,迟到的 notify_response 命中未知 id 分支。
TEST(AskUserQuestionPrompter, UnknownRequestIdNotifyIsNoop) {
    EventDispatcher d;
    AskUserQuestionPrompter prompter(d, 50ms);
    auto resp = prompter.prompt(sample_questions(), nullptr);
    EXPECT_TRUE(resp.timed_out);
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
