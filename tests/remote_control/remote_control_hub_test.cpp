#include <gtest/gtest.h>

#include "remote_control/remote_control_hub.hpp"

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using acecode::rc::InboundResult;
using acecode::rc::OutboundMessage;
using acecode::rc::OutboundSender;
using acecode::rc::RemoteControlHub;

namespace {

// 记录所有投递的假 sender,支持带超时等待第 N 条到达(出站走 hub 内部
// worker 线程,断言前必须等待而不是 sleep 碰运气)。
class FakeSender : public OutboundSender {
public:
    bool send(const OutboundMessage& msg, std::string* /*error*/) override {
        std::lock_guard<std::mutex> lk(mu_);
        sent_.push_back(msg);
        cv_.notify_all();
        return succeed;
    }

    bool wait_for_count(std::size_t n, std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lk(mu_);
        return cv_.wait_for(lk, timeout, [&] { return sent_.size() >= n; });
    }

    std::vector<OutboundMessage> sent() {
        std::lock_guard<std::mutex> lk(mu_);
        return sent_;
    }

    bool succeed = true;

private:
    std::mutex mu_;
    std::condition_variable cv_;
    std::vector<OutboundMessage> sent_;
};

// 可控阻塞的 sender:send 进入后停在闸门上直到 release,用于确定性地制造
// "worker 正在投递中、队列持续堆积"的状态。
class GateSender : public OutboundSender {
public:
    bool send(const OutboundMessage& /*msg*/, std::string* /*error*/) override {
        std::unique_lock<std::mutex> lk(mu_);
        entered_ = true;
        cv_.notify_all();
        cv_.wait(lk, [&] { return released_; });
        return true;
    }

    bool wait_until_entered(std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lk(mu_);
        return cv_.wait_for(lk, timeout, [&] { return entered_; });
    }

    void release() {
        std::lock_guard<std::mutex> lk(mu_);
        released_ = true;
        cv_.notify_all();
    }

private:
    std::mutex mu_;
    std::condition_variable cv_;
    bool entered_ = false;
    bool released_ = false;
};

// 让入站回调在“已被 Hub 接受、实际提交效果尚未发生”的位置停住。测试线程
// 可在此期间原子换路或清路,从而确定性覆盖 rebind/off 竞态而不依赖 sleep。
class InboundCallbackGate {
public:
    void enter_and_wait() {
        std::unique_lock<std::mutex> lk(mu_);
        entered_ = true;
        cv_.notify_all();
        cv_.wait(lk, [&] { return released_; });
    }

    bool wait_until_entered(std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lk(mu_);
        return cv_.wait_for(lk, timeout, [&] { return entered_; });
    }

    void release() {
        std::lock_guard<std::mutex> lk(mu_);
        released_ = true;
        cv_.notify_all();
    }

private:
    std::mutex mu_;
    std::condition_variable cv_;
    bool entered_ = false;
    bool released_ = false;
};

} // namespace

// 场景:hub 未启用时收到入站。期望:拒绝并给出 Disabled 码,统计入 rejected。
TEST(RemoteControlHub, DisabledRejectsInbound) {
    RemoteControlHub hub;
    auto result = hub.handle_inbound("hello", "any-token");
    EXPECT_EQ(result.code, InboundResult::Code::Disabled);
    EXPECT_EQ(hub.stats().inbound_rejected, 1u);
}

// 场景:token 不匹配或缺失。期望:BadToken,且不触发 inbound_submit ——
// remote control 即使来自 loopback 也强制校验 token,任何本机进程都不应能
// 向有工具执行能力的会话注入指令。
TEST(RemoteControlHub, WrongOrMissingTokenRejected) {
    RemoteControlHub hub;
    int submitted = 0;
    hub.set_inbound_submit([&](const std::string&) { ++submitted; });
    hub.enable("secret", "sess-1", nullptr);

    EXPECT_EQ(hub.handle_inbound("hi", "wrong").code, InboundResult::Code::BadToken);
    EXPECT_EQ(hub.handle_inbound("hi", "").code, InboundResult::Code::BadToken);
    EXPECT_EQ(submitted, 0);

    hub.disable();
}

// 场景:token 正确的合法文本。期望:转交 inbound_submit 原文,统计入 accepted。
TEST(RemoteControlHub, ValidInboundReachesSubmit) {
    RemoteControlHub hub;
    std::vector<std::string> received;
    hub.set_inbound_submit([&](const std::string& t) { received.push_back(t); });
    hub.enable("secret", "sess-1", nullptr);

    auto result = hub.handle_inbound(u8"帮我看下这个报错", "secret");
    EXPECT_TRUE(result.ok());
    ASSERT_EQ(received.size(), 1u);
    EXPECT_EQ(received[0], u8"帮我看下这个报错");
    EXPECT_EQ(hub.stats().inbound_accepted, 1u);

    hub.disable();
}

// 场景:合法入站的 submit 回调本身立即排一条出站消息。期望:固定确认已在
// 调用 submit 之前进入同一 FIFO 队列,因此 sender 必须先收到确认、后收到
// submit 回调排入的标记消息。这里断言的是入队顺序,不要求 worker 在 submit
// 返回前完成网络投递。
TEST(RemoteControlHub, ValidInboundQueuesAcknowledgementBeforeSubmit) {
    RemoteControlHub hub;
    auto sender = std::make_shared<FakeSender>();
    hub.enable("secret", "sess-1", sender);
    hub.set_inbound_submit([&](const std::string&) {
        hub.notify_assistant_text("submit-called");
    });

    auto result = hub.handle_inbound("hello", "secret");
    EXPECT_TRUE(result.ok()) << result.message;
    ASSERT_TRUE(sender->wait_for_count(2, std::chrono::seconds(5)));

    hub.disable();
    auto sent = sender->sent();
    ASSERT_EQ(sent.size(), 2u);
    EXPECT_EQ(sent[0].type, "assistant_message");
    EXPECT_EQ(sent[0].session_id, "sess-1");
    EXPECT_EQ(sent[0].text, "思考中...");
    EXPECT_EQ(sent[1].text, "submit-called");
    EXPECT_LT(sent[0].seq, sent[1].seq);
}

// 场景:上一条合法入站对应的工作尚未完成时又收到一条合法入站。期望:每条
// 入站各排一次固定确认,不做 busy/轮次去重。
TEST(RemoteControlHub, EachValidInboundQueuesOneAcknowledgement) {
    RemoteControlHub hub;
    auto sender = std::make_shared<FakeSender>();
    hub.set_inbound_submit([](const std::string&) {});
    hub.enable("secret", "sess-1", sender);

    EXPECT_TRUE(hub.handle_inbound("first", "secret").ok());
    EXPECT_TRUE(hub.handle_inbound("second", "secret").ok());
    ASSERT_TRUE(sender->wait_for_count(2, std::chrono::seconds(5)));

    hub.disable();
    auto sent = sender->sent();
    ASSERT_EQ(sent.size(), 2u);
    EXPECT_EQ(sent[0].text, "思考中...");
    EXPECT_EQ(sent[1].text, "思考中...");
}

// 场景:一条 s1 入站已通过 Hub 校验并进入回调,但回调效果尚未发生时路由
// 原子换绑到 s2。期望:已接受消息的确认和提交仍同属 s1;下一条才走 s2。
TEST(RemoteControlHub, AcceptedInboundKeepsRouteSnapshotAcrossRebind) {
    RemoteControlHub hub;
    auto sender = std::make_shared<FakeSender>();
    hub.enable("secret", "startup", sender);

    InboundCallbackGate gate;
    std::mutex submitted_mu;
    std::vector<std::pair<std::string, std::string>> submitted;
    hub.set_inbound_route("sess-1", [&](const std::string& text) {
        gate.enter_and_wait();
        std::lock_guard<std::mutex> lk(submitted_mu);
        submitted.emplace_back("sess-1", text);
    });

    InboundResult first;
    std::thread inbound([&] {
        first = hub.handle_inbound("first", "secret");
    });
    const bool first_entered = gate.wait_until_entered(std::chrono::seconds(5));
    if (!first_entered) {
        gate.release();
        inbound.join();
        FAIL() << "inbound callback did not reach the deterministic gate";
    }

    hub.set_inbound_route("sess-2", [&](const std::string& text) {
        std::lock_guard<std::mutex> lk(submitted_mu);
        submitted.emplace_back("sess-2", text);
    });
    gate.release();
    inbound.join();
    ASSERT_TRUE(first.ok()) << first.message;

    auto second = hub.handle_inbound("second", "secret");
    ASSERT_TRUE(second.ok()) << second.message;
    ASSERT_TRUE(sender->wait_for_count(2, std::chrono::seconds(5)));

    hub.disable();
    {
        std::lock_guard<std::mutex> lk(submitted_mu);
        ASSERT_EQ(submitted.size(), 2u);
        EXPECT_EQ(submitted[0], std::make_pair(std::string("sess-1"),
                                               std::string("first")));
        EXPECT_EQ(submitted[1], std::make_pair(std::string("sess-2"),
                                               std::string("second")));
    }
    auto sent = sender->sent();
    ASSERT_EQ(sent.size(), 2u);
    EXPECT_EQ(sent[0].session_id, "sess-1");
    EXPECT_EQ(sent[0].text, "思考中...");
    EXPECT_EQ(sent[1].session_id, "sess-2");
    EXPECT_EQ(sent[1].text, "思考中...");
}

// 场景:合法入站已被接受后立刻清路(等价于 /rc off 的路由切断)。期望:这条
// 已接受消息仍完成原会话提交;清路之后的新消息才返回 NoSession 且无确认。
TEST(RemoteControlHub, AcceptedInboundSurvivesImmediateRouteClear) {
    RemoteControlHub hub;
    auto sender = std::make_shared<FakeSender>();
    hub.enable("secret", "startup", sender);

    InboundCallbackGate gate;
    std::mutex submitted_mu;
    std::vector<std::pair<std::string, std::string>> submitted;
    hub.set_inbound_route("sess-1", [&](const std::string& text) {
        gate.enter_and_wait();
        std::lock_guard<std::mutex> lk(submitted_mu);
        submitted.emplace_back("sess-1", text);
    });

    InboundResult accepted;
    std::thread inbound([&] {
        accepted = hub.handle_inbound("accepted-before-off", "secret");
    });
    const bool accepted_entered =
        gate.wait_until_entered(std::chrono::seconds(5));
    if (!accepted_entered) {
        gate.release();
        inbound.join();
        FAIL() << "inbound callback did not reach the deterministic gate";
    }
    hub.clear_inbound_route();
    gate.release();
    inbound.join();

    ASSERT_TRUE(accepted.ok()) << accepted.message;
    EXPECT_EQ(hub.handle_inbound("after-off", "secret").code,
              InboundResult::Code::NoSession);
    ASSERT_TRUE(sender->wait_for_count(1, std::chrono::seconds(5)));

    hub.disable();
    {
        std::lock_guard<std::mutex> lk(submitted_mu);
        ASSERT_EQ(submitted.size(), 1u);
        EXPECT_EQ(submitted[0],
                  std::make_pair(std::string("sess-1"),
                                 std::string("accepted-before-off")));
    }
    auto sent = sender->sent();
    ASSERT_EQ(sent.size(), 1u);
    EXPECT_EQ(sent[0].session_id, "sess-1");
    EXPECT_EQ(sent[0].text, "思考中...");
}

// 场景:接受入站时 webhook sender 尚未安装。期望:确认不丢失,稍后安装 sender
// 后按接受瞬间的 route session_id 投递。
TEST(RemoteControlHub, SenderNullDelaysAcknowledgementUntilSenderInstalled) {
    RemoteControlHub hub;
    int submitted = 0;
    hub.enable("secret", "startup", nullptr);
    hub.set_inbound_route("sess-delayed", [&](const std::string&) { ++submitted; });

    auto result = hub.handle_inbound("hello", "secret");
    ASSERT_TRUE(result.ok()) << result.message;
    EXPECT_EQ(submitted, 1);

    auto sender = std::make_shared<FakeSender>();
    hub.set_outbound_sender(sender);
    ASSERT_TRUE(sender->wait_for_count(1, std::chrono::seconds(5)));
    hub.disable();

    auto sent = sender->sent();
    ASSERT_EQ(sent.size(), 1u);
    EXPECT_EQ(sent[0].session_id, "sess-delayed");
    EXPECT_EQ(sent[0].text, "思考中...");
}

// 场景:disabled 与 oversize 两条拒绝路径。期望:既不提交也不悄悄消耗
// 出站 seq;后续 control 必须仍是首条消息(seq=1),从而证明没有隐藏确认。
TEST(RemoteControlHub, DisabledAndOversizeDoNotQueueAcknowledgement) {
    RemoteControlHub hub;
    auto sender = std::make_shared<FakeSender>();
    int submitted = 0;
    hub.set_inbound_route("sess-1", [&](const std::string&) { ++submitted; });

    EXPECT_EQ(hub.handle_inbound("while-disabled", "secret").code,
              InboundResult::Code::Disabled);

    hub.enable("secret", "sess-1", sender);
    std::string huge(RemoteControlHub::kMaxInboundBytes + 1, 'x');
    EXPECT_EQ(hub.handle_inbound(huge, "secret").code,
              InboundResult::Code::BadText);
    EXPECT_EQ(submitted, 0);

    hub.notify_assistant_text("control");
    ASSERT_TRUE(sender->wait_for_count(1, std::chrono::seconds(5)));
    hub.disable();

    auto sent = sender->sent();
    ASSERT_EQ(sent.size(), 1u);
    EXPECT_EQ(sent[0].seq, 1u);
    EXPECT_EQ(sent[0].text, "control");
}

// 场景:Disabled/BadToken/BadText/NoSession 入站。期望:全部拒绝且不排固定
// 确认。最后主动排一条 control 并 join worker,据 sender 最终精确条数证明
// 前面的拒绝路径没有留下异步消息。
TEST(RemoteControlHub, RejectedInboundDoesNotQueueAcknowledgement) {
    RemoteControlHub hub;
    EXPECT_EQ(hub.handle_inbound("disabled", "secret").code,
              InboundResult::Code::Disabled);

    auto sender = std::make_shared<FakeSender>();
    hub.enable("secret", "sess-1", sender);
    EXPECT_EQ(hub.handle_inbound("bad token", "wrong").code,
              InboundResult::Code::BadToken);
    EXPECT_EQ(hub.handle_inbound("   ", "secret").code,
              InboundResult::Code::BadText);
    EXPECT_EQ(hub.handle_inbound("no session", "secret").code,
              InboundResult::Code::NoSession);

    hub.notify_assistant_text("control");
    ASSERT_TRUE(sender->wait_for_count(1, std::chrono::seconds(5)));
    hub.disable();

    auto sent = sender->sent();
    ASSERT_EQ(sent.size(), 1u);
    EXPECT_EQ(sent[0].text, "control");
}

// 场景:空文本 / 纯空白 / 超过 kMaxInboundBytes 的超大文本。期望:BadText。
// 上限只防恶意或失控 payload,正常 IM 消息远小于 64KB。
TEST(RemoteControlHub, BlankAndOversizeTextRejected) {
    RemoteControlHub hub;
    hub.set_inbound_submit([](const std::string&) {});
    hub.enable("secret", "sess-1", nullptr);

    EXPECT_EQ(hub.handle_inbound("", "secret").code, InboundResult::Code::BadText);
    EXPECT_EQ(hub.handle_inbound("  \n\t ", "secret").code, InboundResult::Code::BadText);
    std::string huge(RemoteControlHub::kMaxInboundBytes + 1, 'x');
    EXPECT_EQ(hub.handle_inbound(huge, "secret").code, InboundResult::Code::BadText);

    hub.disable();
}

// 场景:出站投递。期望:notify 的文本经 worker 线程到达 sender,seq 单调递增,
// payload JSON 含 type/session_id/text/timestamp_ms/seq 五个字段。
TEST(RemoteControlHub, OutboundDeliversInOrderWithSeq) {
    RemoteControlHub hub;
    auto sender = std::make_shared<FakeSender>();
    hub.enable("secret", "sess-9", sender);

    hub.notify_assistant_text("first");
    hub.notify_assistant_text("second");
    ASSERT_TRUE(sender->wait_for_count(2, std::chrono::seconds(5)));

    auto sent = sender->sent();
    ASSERT_EQ(sent.size(), 2u);
    EXPECT_EQ(sent[0].text, "first");
    EXPECT_EQ(sent[1].text, "second");
    EXPECT_LT(sent[0].seq, sent[1].seq);
    EXPECT_EQ(sent[0].session_id, "sess-9");

    auto j = acecode::rc::outbound_message_to_json(sent[0]);
    EXPECT_EQ(j["type"], "assistant_message");
    EXPECT_EQ(j["session_id"], "sess-9");
    EXPECT_EQ(j["text"], "first");
    EXPECT_TRUE(j.contains("timestamp_ms"));
    EXPECT_TRUE(j.contains("seq"));

    // stats 断言必须放在 disable(join worker)之后:sender 收到消息的时刻
    // worker 还没从 send() 返回,stats 更新发生在其后,先断言会偶发读到旧值。
    hub.disable();
    EXPECT_EQ(hub.stats().outbound_sent, 2u);
}

// 场景:sender 为 null(出站 webhook 未配置)或文本为空白。期望:静默忽略,
// 不入队也不崩溃 —— 仅入站形态是合法配置。
TEST(RemoteControlHub, OutboundWithoutSenderOrBlankTextIsNoop) {
    RemoteControlHub hub;
    hub.enable("secret", "sess-1", nullptr);
    hub.notify_assistant_text("ignored");

    auto sender = std::make_shared<FakeSender>();
    hub.set_outbound_sender(sender);
    hub.notify_assistant_text("   ");
    hub.notify_assistant_text("real");
    ASSERT_TRUE(sender->wait_for_count(1, std::chrono::seconds(5)));
    EXPECT_EQ(sender->sent().size(), 1u);
    EXPECT_EQ(sender->sent()[0].text, "real");

    hub.disable();
}

// 场景:channel bridge 长时间不可达(sender 阻塞),出站持续堆积超过 kMaxQueue。
// 期望:丢最旧的并计入 outbound_dropped,而不是无界占用内存。
TEST(RemoteControlHub, OutboundQueueOverflowDropsOldest) {
    RemoteControlHub hub;
    auto gate = std::make_shared<GateSender>();
    hub.enable("secret", "sess-1", gate);

    // 先送一条并等 worker 真正进入 send 阻塞:此后队列状态完全由本线程控制,
    // 溢出计数才是确定值。
    hub.notify_assistant_text("in-flight");
    ASSERT_TRUE(gate->wait_until_entered(std::chrono::seconds(5)));

    const std::size_t overflow = 5;
    for (std::size_t i = 0; i < RemoteControlHub::kMaxQueue + overflow; ++i) {
        hub.notify_assistant_text("msg-" + std::to_string(i));
    }
    EXPECT_EQ(hub.stats().outbound_dropped, overflow);

    gate->release();
    hub.disable();
}

// 场景:投递失败(channel bridge 返回错误)。期望:计入 outbound_failed,worker 继续
// 处理后续消息而不是停摆。
TEST(RemoteControlHub, OutboundFailureCountedAndWorkerContinues) {
    RemoteControlHub hub;
    auto sender = std::make_shared<FakeSender>();
    sender->succeed = false;
    hub.enable("secret", "sess-1", sender);

    hub.notify_assistant_text("a");
    hub.notify_assistant_text("b");
    ASSERT_TRUE(sender->wait_for_count(2, std::chrono::seconds(5)));
    // 同 OutboundDeliversInOrderWithSeq:stats 在 send() 返回后才更新,
    // 必须先 disable(join worker)再断言。
    hub.disable();
    EXPECT_EQ(hub.stats().outbound_failed, 2u);
    EXPECT_EQ(hub.stats().outbound_sent, 0u);
}

// 场景:disable 后再次 handle_inbound;以及重复 enable。期望:disable 拒绝
// 入站;重复 enable 替换 token 并继续工作(worker 线程重建无悬挂)。
TEST(RemoteControlHub, DisableThenReenableSwapsToken) {
    RemoteControlHub hub;
    int submitted = 0;
    hub.set_inbound_submit([&](const std::string&) { ++submitted; });

    hub.enable("token-a", "sess-1", nullptr);
    EXPECT_TRUE(hub.handle_inbound("x", "token-a").ok());
    hub.disable();
    EXPECT_EQ(hub.handle_inbound("x", "token-a").code, InboundResult::Code::Disabled);

    hub.enable("token-b", "sess-1", nullptr);
    EXPECT_EQ(hub.handle_inbound("x", "token-a").code, InboundResult::Code::BadToken);
    EXPECT_TRUE(hub.handle_inbound("x", "token-b").ok());
    EXPECT_EQ(submitted, 2);
    hub.disable();
}

// 场景:转发游标读写(TUI 回合结束转发循环依赖它)。期望:set/get 一致。
TEST(RemoteControlHub, ForwardCursorRoundTrips) {
    RemoteControlHub hub;
    EXPECT_EQ(hub.forward_cursor(), 0u);
    hub.set_forward_cursor(42);
    EXPECT_EQ(hub.forward_cursor(), 42u);
}

// 场景:agent 回合内发生工具调用。期望:走同一条出站队列/worker 路径到达
// sender,payload JSON 的 type 是 "tool_call"、含 tool_name/args_preview,
// 且不含 text 键(tool_call 消息没有 text,序列化时应省略该键而不是输出
// 空字符串)。
TEST(RemoteControlHub, NotifyToolCallDeliversToolCallJson) {
    RemoteControlHub hub;
    auto sender = std::make_shared<FakeSender>();
    hub.enable("secret", "sess-9", sender);

    nlohmann::json args = {{"command", "ls -la"}};
    hub.notify_tool_call("sess-9", "bash", args);
    ASSERT_TRUE(sender->wait_for_count(1, std::chrono::seconds(5)));

    auto sent = sender->sent();
    ASSERT_EQ(sent.size(), 1u);
    EXPECT_EQ(sent[0].type, "tool_call");
    EXPECT_EQ(sent[0].session_id, "sess-9");
    EXPECT_EQ(sent[0].tool_name, "bash");
    EXPECT_EQ(sent[0].args_preview, "ls -la");
    EXPECT_TRUE(sent[0].text.empty());

    auto j = acecode::rc::outbound_message_to_json(sent[0]);
    EXPECT_EQ(j["type"], "tool_call");
    EXPECT_EQ(j["tool_name"], "bash");
    EXPECT_EQ(j["args_preview"], "ls -la");
    EXPECT_FALSE(j.contains("text"));
    EXPECT_FALSE(j.contains("in_reply_to"));

    hub.disable();
}

// 场景:hub 未启用或无 sender 时发生工具调用。期望:静默忽略,不入队也不
// 崩溃 —— 与 notify_assistant_text 的 no-op 语义一致。
TEST(RemoteControlHub, NotifyToolCallWithoutSenderIsNoop) {
    RemoteControlHub hub;
    hub.enable("secret", "sess-1", nullptr);
    hub.notify_tool_call("sess-1", "bash", nlohmann::json{{"command", "ignored"}});

    auto sender = std::make_shared<FakeSender>();
    hub.set_outbound_sender(sender);
    hub.notify_tool_call("sess-1", "bash", nlohmann::json{{"command", "real"}});
    ASSERT_TRUE(sender->wait_for_count(1, std::chrono::seconds(5)));
    EXPECT_EQ(sender->sent().size(), 1u);
    EXPECT_EQ(sender->sent()[0].args_preview, "real");

    hub.disable();
}

// 场景:outbound_message_to_json 序列化留空的可选字段。期望:空 in_reply_to
// (以及空 tool_name/args_preview)不输出对应键;非空 in_reply_to 正常输出。
TEST(RemoteControlHub, ToJsonOmitsEmptyOptionalFields) {
    OutboundMessage msg;
    msg.type = "assistant_message";
    msg.session_id = "sess-1";
    msg.text = "hello";
    msg.timestamp_ms = 123;
    msg.seq = 1;
    // in_reply_to / tool_name / args_preview 保持默认空字符串。

    auto j = acecode::rc::outbound_message_to_json(msg);
    EXPECT_FALSE(j.contains("in_reply_to"));
    EXPECT_FALSE(j.contains("tool_name"));
    EXPECT_FALSE(j.contains("args_preview"));

    msg.in_reply_to = "inbound-42";
    auto j2 = acecode::rc::outbound_message_to_json(msg);
    ASSERT_TRUE(j2.contains("in_reply_to"));
    EXPECT_EQ(j2["in_reply_to"], "inbound-42");
}

// 场景:daemon 换绑会话后调用 set_session_id。期望:后续 notify_assistant_text
// 的出站消息 session_id 立即切换为新会话 —— 出站归属必须跟随当前绑定,
// 未绑定会话的名义不允许残留在 channel payload 上。
TEST(RemoteControlHub, SetSessionIdRetagsSubsequentOutbound) {
    RemoteControlHub hub;
    auto sender = std::make_shared<FakeSender>();
    hub.enable("secret", "sess-old", sender);

    hub.notify_assistant_text("from old");
    ASSERT_TRUE(sender->wait_for_count(1, std::chrono::seconds(5)));

    hub.set_session_id("sess-new");
    hub.notify_assistant_text("from new");
    ASSERT_TRUE(sender->wait_for_count(2, std::chrono::seconds(5)));

    auto sent = sender->sent();
    ASSERT_EQ(sent.size(), 2u);
    EXPECT_EQ(sent[0].session_id, "sess-old");
    EXPECT_EQ(sent[1].session_id, "sess-new");

    hub.disable();
}

// 场景:注册出站结果观察者后投递成功与失败。期望:每次投递(不论成败)都
// 回调一次,并携带真实结果 —— daemon 保活判定(连续失败阈值)依赖这个信号。
TEST(RemoteControlHub, OutboundResultObserverSeesEachDelivery) {
    RemoteControlHub hub;
    auto sender = std::make_shared<FakeSender>();

    std::mutex mu;
    std::condition_variable cv;
    std::vector<bool> results;
    hub.set_outbound_result_observer([&](bool ok) {
        std::lock_guard<std::mutex> lk(mu);
        results.push_back(ok);
        cv.notify_all();
    });

    hub.enable("secret", "sess-1", sender);

    sender->succeed = false;
    hub.notify_assistant_text("will fail");
    {
        std::unique_lock<std::mutex> lk(mu);
        ASSERT_TRUE(cv.wait_for(lk, std::chrono::seconds(5),
                                [&] { return results.size() >= 1; }));
    }

    sender->succeed = true;
    hub.notify_assistant_text("will pass");
    {
        std::unique_lock<std::mutex> lk(mu);
        ASSERT_TRUE(cv.wait_for(lk, std::chrono::seconds(5),
                                [&] { return results.size() >= 2; }));
        ASSERT_EQ(results.size(), 2u);
        EXPECT_FALSE(results[0]);
        EXPECT_TRUE(results[1]);
    }

    // 观察者可清除;清除后继续投递不崩溃。
    hub.set_outbound_result_observer({});
    hub.notify_assistant_text("no observer");
    ASSERT_TRUE(sender->wait_for_count(3, std::chrono::seconds(5)));

    hub.disable();
}
