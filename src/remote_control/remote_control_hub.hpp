#pragma once

// Remote control 基座的核心状态机(openspec add-remote-control)。
//
// 职责边界:
//   - 入站(channel → ACECode):校验 token / 文本,合法则先排一条固定确认,
//     再转交 inbound_submit 回调。回调由 main.cpp 接到输入框同款的提交路径
//     (busy 时排队,空闲时 submit)。
//   - 出站(ACECode → channel):assistant 回合产出经 notify_assistant_text 进有界
//     队列,由 hub 自有 worker 线程异步调 OutboundSender 投递 —— 投递阻塞或
//     失败都不影响 agent / UI 线程。
//
// 本类零网络依赖,网络面(loopback HTTP listener / webhook POST)在
// remote_control_service 中组装。换 channel 通道时实现新的 OutboundSender 即可,
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
    std::string type;        // "assistant_message" 或 "tool_call"
    std::string session_id;
    std::string text;         // assistant_message 用;tool_call 留空
    std::string tool_name;    // tool_call 用;assistant_message 留空
    std::string args_preview; // tool_call 用,summarize_tool_args 的结果
    std::string in_reply_to;  // 预留:回复某条 inbound 消息时填其标识
    std::int64_t timestamp_ms = 0;
    std::uint64_t seq = 0;   // hub 内单调递增;channel bridge 可据此去重/排序
};

nlohmann::json outbound_message_to_json(const OutboundMessage& msg);

// 出站投递抽象 —— "已 hook 的 channel 发送方法"的接缝。默认实现是
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
    std::uint64_t outbound_dropped = 0;  // 队列满被丢弃(channel bridge 长时间不可达)
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

    // main.cpp 启动期注册一次;把 channel 来的文本注入当前 TUI 会话。
    void set_inbound_submit(InboundSubmit fn);

    // daemon channel 绑定专用:在同一把 Hub 锁下发布“目标会话 + 提交回调”。
    // handle_inbound 会把这一对作为一个路由快照使用,因此随后发生 rebind/off
    // 也不会让已接受消息的确认与实际提交分属两个会话。空 session 或空回调
    // 等价于 clear_inbound_route()。
    void set_inbound_route(std::string session_id, InboundSubmit fn);
    // 切断后续入站路由，并把调用瞬间之前已排队的出站消息作为一个
    // drain-through barrier：若 worker 和 sender 都可用，等到该时刻的
    // 最后一条消息已被 worker 从 FIFO 接管再返回。这样紧随其后的
    // disable() 会 join 正在 send 的 worker，而不是先清掉已确认入站的 ack。
    // 不等待网络投递；sender/worker 不可用时安全地立即返回。
    void clear_inbound_route();

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

    // 换绑会话时更新出站消息的 session 归属(daemon 托管模式:/rc 换绑不
    // 重启服务,但后续 notify_assistant_text 必须立即以新会话名义出站)。
    void set_session_id(std::string session_id);

    // 出站投递结果观察者:worker 线程每次 send 后(不论成败)锁外回调一次。
    // daemon 保活判定(连续失败 ≥N 触发幂等再激活)依赖这个信号;传空清除。
    using OutboundResultObserver = std::function<void(bool ok)>;
    void set_outbound_result_observer(OutboundResultObserver observer);

    // HTTP listener 调用。注意:与 daemon 的"loopback 免 token"不同,remote
    // control 即使来自 loopback 也强制校验 token —— 任何本机进程都不应能向一个
    // 有工具执行能力的 agent 会话注入指令。
    InboundResult handle_inbound(const std::string& text,
                                 const std::string& provided_token);

    // 回合结束时 TUI 调用;文本进出站队列,立即返回。
    void notify_assistant_text(const std::string& text);

    // agent 回合内发生工具调用时调用;tool_name + arguments 的摘要进出站
    // 队列,立即返回。复用 notify_assistant_text 同一条有界队列/worker 线程
    // 路径。session_id 由调用方显式传入(不读 enable() 时记录的
    // session_id_),为将来按会话归属出站消息留口子。
    void notify_tool_call(const std::string& session_id,
                          const std::string& tool_name,
                          const nlohmann::json& arguments);

    // TUI conversation 的转发游标:enable 时由命令置为当前对话长度(不回放
    // 历史),回合结束的转发循环读写。语义上属于 TUI 侧,放在 hub 仅为跨
    // 闭包共享 —— main.cpp 的 callbacks 与 slash command 无共同作用域。
    void set_forward_cursor(std::size_t index);
    std::size_t forward_cursor() const;

    RemoteControlStats stats() const;

    // 入站文本上限。channel 消息正常远小于此;上限只防恶意/失控的超大 payload。
    static constexpr std::size_t kMaxInboundBytes = 64 * 1024;
    // 出站队列上限。channel bridge 不可达时丢最旧的而不是无界堆积。
    static constexpr std::size_t kMaxQueue = 256;

private:
    // 调用方须持 mu_ 且已完成 enabled/text 等校验。只负责构造
    // assistant_message 并放入既有有界 FIFO;允许 sender_ 暂为空,worker 会
    // 在 set_outbound_sender 后异步投递。
    void enqueue_assistant_text_locked(const std::string& text,
                                       const std::string& session_id);
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
    OutboundResultObserver outbound_result_observer_;
    std::deque<OutboundMessage> queue_;
    std::thread worker_;
    std::uint64_t next_seq_ = 1;
    std::uint64_t last_dequeued_seq_ = 0;
    // 非零时，入队端不得因 FIFO 满而淘汰该 seq 及之前的消息；否则
    // clear_inbound_route 的 barrier 可能永远等不到自己保护的消息。
    std::uint64_t drain_through_seq_ = 0;
    std::size_t forward_cursor_ = 0;
    RemoteControlStats stats_;
};

} // namespace acecode::rc
