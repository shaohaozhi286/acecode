#pragma once

// SessionClient: 会话客户端抽象层(openspec add-web-daemon Section 7)。
//
// 一个 SessionClient 给"上层调用者"(daemon HTTP handler / 浏览器侧 RemoteClient
// / 未来 IDE 插件等)提供统一接口去管理 AgentLoop 实例:create/list/destroy +
// 订阅事件流 + 发输入 + 回应权限请求 + abort。
//
// v1 只有一个实现 LocalSessionClient(同进程,直接持有 SessionRegistry)。
// 浏览器侧的 RemoteSessionClient 由前端 change(add-web-chat-ui)落地。
//
// 设计原则:
//   - 接口 hpp 不依赖任何具体实现头(只 string + json + functional + 标准库)
//   - SessionEvent 是事件流的最小公共表示;AgentLoop 内部产物经 adapter 转过来
//   - SessionClient 方法可阻塞(create/list);事件订阅是 push 模式(回调)

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace acecode {

// ----- 事件流 -----

// SessionEvent::Kind 与 spec design.md WS 协议的 type 一一对应。
// 服务端 → 客户端方向。
enum class SessionEventKind {
    Token,             // payload: {"text": "..."}
    Reasoning,         // payload: {"text": "..."} (DeepSeek thinking 等)
    Message,           // payload: 一条完整 ChatMessage(JSON 序列化形式)
    ToolStart,         // payload: {"tool":"...", "command_preview":"..."}
    ToolUpdate,        // payload: {"tool":"...", "tail":[...], "partial":"...", "total_lines":N, "total_bytes":N}
    ToolEnd,           // payload: {"tool":"...", "result_summary": {...}, "ok": bool}
    PermissionRequest, // payload: {"request_id":"...", "tool":"...", "args": {...}}
    QuestionRequest,   // payload: {"request_id":"...", "questions":[...]} (AskUserQuestion 工具)
    Usage,             // payload: {"input": N, "output": N, ...}
    BusyChanged,       // payload: {"busy": bool}
    Done,              // payload: {} —— 一轮 agent loop 结束
    Error,             // payload: {"reason":"...", "request_id":"..."(可选)}
};

struct SessionEvent {
    SessionEventKind kind;
    std::uint64_t    seq = 0;        // 该 session 内单调递增,从 1 开始
    std::int64_t     timestamp_ms = 0;
    nlohmann::json   payload;
};

// ----- 客户端 → 服务端的命令 -----

enum class PermissionDecisionChoice {
    Allow,
    Deny,
    AllowSession, // = AlwaysAllow,这次允许 + 本 session 内不再问
};

struct PermissionDecision {
    std::string request_id;
    PermissionDecisionChoice choice = PermissionDecisionChoice::Deny;
};

// ----- Session 创建参数 -----

struct SessionOptions {
    // 可选 model override(对应 saved_models.name 或 "(legacy)")。
    // 留空 = 用 daemon 启动时的 default。
    std::string model_name;

    // 可选初始系统消息或 prompt 注入。v1 留空。
    std::string initial_user_message;

    // 是否在 session 创建后立刻启动 agent loop 处理 initial_user_message。
    // 留 false 让客户端控制时机。
    bool auto_start = false;
};

// ----- Session 元数据(给 list_sessions 用) -----

struct SessionInfo {
    std::string id;
    std::string created_at;       // ISO 8601
    std::string updated_at;
    std::string summary;          // 最后一条 user 消息的截断
    std::string provider;
    std::string model;
    std::string title;
    int         message_count = 0;
    bool        active = false;   // 是否在 SessionRegistry 内存活
};

// ----- AskUserQuestion 回应(client→server) -----

// 注: 完整的结构体定义在 ask_user_question_prompter.hpp。这里 fwd 声明,
// 让 SessionClient 接口不强依赖 prompter 头(它属于 daemon 实现细节)。
struct AskUserQuestionResponse;

// ----- 主接口 -----

class SessionClient {
public:
    using EventListener = std::function<void(const SessionEvent&)>;
    using SubscriptionId = std::uint64_t;

    virtual ~SessionClient() = default;

    // 创建一个新 session,返回 session_id。同步阻塞直到 SessionRegistry 完成
    // 注册 + AgentLoop 起线程。
    virtual std::string create_session(const SessionOptions& opts) = 0;

    // 从当前 cwd 的磁盘历史恢复一个 session 到内存 registry。若该 id 已经
    // active,直接返回 true,不在同一 daemon 内创建第二份同 id 上下文。
    virtual bool resume_session(const std::string& id) = 0;

    // 列出当前 daemon 内的 session(内存活跃 + 磁盘历史合并去重)。
    virtual std::vector<SessionInfo> list_sessions() = 0;

    // 销毁 session: abort 当前轮 + join worker + 从 registry 移除。
    // 不会删除磁盘上的 jsonl/meta 文件(那是 cleanup_old_sessions 的事)。
    virtual void destroy_session(const std::string& id) = 0;

    // 订阅事件流。`since_seq` > 0 时,先回放缓存里 seq > since_seq 的旧事件
    // (供断线重连补齐),再继续推实时新事件。返回 SubscriptionId 用于退订。
    virtual SubscriptionId subscribe(const std::string& session_id,
                                       EventListener on_event,
                                       std::uint64_t since_seq = 0) = 0;

    // 退订(线程安全)。
    virtual void unsubscribe(const std::string& session_id, SubscriptionId sub) = 0;

    // 发送一条用户输入。非阻塞,内部入队到 AgentLoop worker。
    virtual void send_input(const std::string& session_id, const std::string& text) = 0;

    // 回应一个之前推送的 permission_request。线程安全。
    // 未知 request_id / 已超时的请求 = no-op。
    virtual void respond_permission(const std::string& session_id,
                                     const PermissionDecision& decision) = 0;

    // 回应一个之前推送的 question_request(AskUserQuestion 工具)。线程安全。
    // 未知 request_id / 已超时的请求 = no-op。
    virtual void respond_question(const std::string& session_id,
                                    const std::string& request_id,
                                    const AskUserQuestionResponse& response) = 0;

    // 请求中止当前轮(不销毁 session)。
    virtual void abort(const std::string& session_id) = 0;
};

// ----- helpers (header-only) -----

inline const char* to_string(SessionEventKind k) {
    switch (k) {
        case SessionEventKind::Token:             return "token";
        case SessionEventKind::Reasoning:         return "reasoning";
        case SessionEventKind::Message:           return "message";
        case SessionEventKind::ToolStart:         return "tool_start";
        case SessionEventKind::ToolUpdate:        return "tool_update";
        case SessionEventKind::ToolEnd:           return "tool_end";
        case SessionEventKind::PermissionRequest: return "permission_request";
        case SessionEventKind::QuestionRequest:   return "question_request";
        case SessionEventKind::Usage:             return "usage";
        case SessionEventKind::BusyChanged:       return "busy_changed";
        case SessionEventKind::Done:              return "done";
        case SessionEventKind::Error:             return "error";
    }
    return "unknown";
}

inline const char* to_string(PermissionDecisionChoice c) {
    switch (c) {
        case PermissionDecisionChoice::Allow:        return "allow";
        case PermissionDecisionChoice::Deny:         return "deny";
        case PermissionDecisionChoice::AllowSession: return "allow_session";
    }
    return "deny";
}

inline std::optional<PermissionDecisionChoice>
parse_permission_choice(const std::string& s) {
    if (s == "allow")         return PermissionDecisionChoice::Allow;
    if (s == "deny")          return PermissionDecisionChoice::Deny;
    if (s == "allow_session") return PermissionDecisionChoice::AllowSession;
    return std::nullopt;
}

} // namespace acecode
