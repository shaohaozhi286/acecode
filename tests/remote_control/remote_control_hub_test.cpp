#include <gtest/gtest.h>

#include "remote_control/remote_control_hub.hpp"

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string>
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
