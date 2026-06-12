#pragma once

// Remote control 基座的核心状态机(openspec add-remote-control)。
//
// 职责边界:
//   - 入站(IM → ACECode):校验 token / 文本,合法则转交 inbound_submit 回调。
//     回调由 main.cpp 接到输入框同款的提交路径(busy 时排队,空闲时 submit)。
//   - 出站(ACECode → IM):assistant 回合产出经 notify_assistant_text 进有界
//     队列,由 hub 自有 worker 线程异步调 OutboundSender 投递 —— 投递阻塞或
//     失败都不影响 agent / UI 线程。
//
// 本类零网络依赖,网络面(loopback HTTP listener / webhook POST)在
// remote_control_service 中组装。换 IM 通道时实现新的 OutboundSender 即可,
// hub 不动。
//
// 线程模型:
//   - enable/disable/状态读取:UI 线程(slash command)
//   - handle_inbound:HTTP listener worker 线程
//   - notify_assistant_text:agent worker 线程(on_busy_changed 回调内)
//   - OutboundSender::send:hub 内部 worker 线程
// 所有共享状态由单一 mu_ 保护;回调在锁外调用,避免与 TUI state.mu 死锁。

#include <nlohmann/json.hpp>

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace acecode::rc {

struct OutboundMessage {
    std::string type;        // 目前固定 "assistant_message"
    std::string session_id;
    std::string text;
    std::int64_t timestamp_ms = 0;
    std::uint64_t seq = 0;   // hub 内单调递增;IM 桥可据此去重/排序
};

nlohmann::json outbound_message_to_json(const OutboundMessage& msg);

// 出站投递抽象 —— "已 hook 的 IM 发送方法"的接缝。默认实现是
// remote_control_service 里的 webhook POST;接入其它通道时实现本接口。
class OutboundSender {
public:
    virtual ~OutboundSender() = default;
    // 同步投递一条消息。失败返回 false 并填 error(用于统计与日志)。
    virtual bool send(const OutboundMessage& msg, std::string* error) = 0;
};

struct RemoteControlStats {
    std::uint64_t inbound_accepted = 0;
    std::uint64_t inbound_rejected = 0;
    std::uint64_t outbound_sent = 0;
    std::uint64_t outbound_failed = 0;
    std::uint64_t outbound_dropped = 0;  // 队列满被丢弃(IM 桥长时间不可达)
};

struct InboundResult {
    enum class Code { Ok, Disabled, BadToken, BadText, NoSession };
    Code code = Code::Ok;
    std::string message;  // code != Ok 时的人类可读原因
    bool ok() const { return code == Code::Ok; }
};

class RemoteControlHub {
public:
    using InboundSubmit = std::function<void(const std::string& text)>;

    RemoteControlHub() = default;
    ~RemoteControlHub();

    RemoteControlHub(const RemoteControlHub&) = delete;
    RemoteControlHub& operator=(const RemoteControlHub&) = delete;

    // main.cpp 启动期注册一次;把 IM 来的文本注入当前 TUI 会话。
    void set_inbound_submit(InboundSubmit fn);

    // 启用:记录 token / session,启动出站 worker。sender 允许为 null(仅入站,
    // 出站 webhook 未配置时的形态)。重复 enable 会先 disable 再重建。
    void enable(std::string token,
                std::string session_id,
                std::shared_ptr<OutboundSender> sender);
    void disable();
    bool enabled() const;
    std::string token() const;

    // 运行期替换出站通道(/remote-control url <u> 热更新)。
    void set_outbound_sender(std::shared_ptr<OutboundSender> sender);

    // HTTP listener 调用。注意:与 daemon 的"loopback 免 token"不同,remote
    // control 即使来自 loopback 也强制校验 token —— 任何本机进程都不应能向一个
    // 有工具执行能力的 agent 会话注入指令。
    InboundResult handle_inbound(const std::string& text,
                                 const std::string& provided_token);

    // 回合结束时 TUI 调用;文本进出站队列,立即返回。
    void notify_assistant_text(const std::string& text);

    // TUI conversation 的转发游标:enable 时由命令置为当前对话长度(不回放
    // 历史),回合结束的转发循环读写。语义上属于 TUI 侧,放在 hub 仅为跨
    // 闭包共享 —— main.cpp 的 callbacks 与 slash command 无共同作用域。
    void set_forward_cursor(std::size_t index);
    std::size_t forward_cursor() const;

    RemoteControlStats stats() const;

    // 入站文本上限。IM 消息正常远小于此;上限只防恶意/失控的超大 payload。
    static constexpr std::size_t kMaxInboundBytes = 64 * 1024;
    // 出站队列上限。IM 桥不可达时丢最旧的而不是无界堆积。
    static constexpr std::size_t kMaxQueue = 256;

private:
    void worker_loop();
    void stop_worker_locked(std::unique_lock<std::mutex>& lk);

    mutable std::mutex mu_;
    std::condition_variable cv_;
    bool enabled_ = false;
    bool stopping_ = false;
    std::string token_;
    std::string session_id_;
    InboundSubmit inbound_submit_;
    std::shared_ptr<OutboundSender> sender_;
    std::deque<OutboundMessage> queue_;
    std::thread worker_;
    std::uint64_t next_seq_ = 1;
    std::size_t forward_cursor_ = 0;
    RemoteControlStats stats_;
};

} // namespace acecode::rc
