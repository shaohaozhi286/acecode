#pragma once

// AskUserQuestionPrompter: 把 LLM 调 AskUserQuestion 工具的"等用户回答"
// 抽出来,给 daemon 路径用。TUI 路径继续走 ask_user_question_tool.cpp 里
// 直接操作 TuiState 的 overlay 流程,本类不参与。
//
// 模式照抄 permission_prompter.cpp::AsyncPrompter:
//   - prompt() 推一个 QuestionRequest 事件到 EventDispatcher,然后 condvar
//     等 respond(request_id, response) 到来
//   - 5 分钟超时视为 cancelled,工具返回 "user cancelled"
//   - abort_flag 50ms 轮询(避免 worker thread 卡 5min 无法 join)
//   - 每次 prompt 生成新 request_id,响应通过 id 路由,与 permission_prompter
//     的 id 空间互不干扰(各自独立的 unordered_map)
//
// 调用方契约:
//   - tool 侧 prompt(...) 拿回 AskUserQuestionResponse,自己拼 ToolResult
//   - WS 侧收到 question_answer 消息后调 notify_response

#include "event_dispatcher.hpp"
#include "session_client.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace acecode {

// 一个问题的答案。selected 装 option label;multiSelect=false 时只会有 1 项,
// =true 时 0..n 项。custom_text 非空表示走了 "Other..." 自定义路径(可与
// selected 共存,前端决定;tool 侧拼接时按 ", " 合并)。
struct AskUserQuestionAnswer {
    std::string              question_id; // 等价于 question 文本(沿用 TUI 行为)
    std::vector<std::string> selected;
    std::string              custom_text;
};

// 一次 AskUserQuestion 调用的整批回答(每个 question 一项),或者 cancelled。
struct AskUserQuestionResponse {
    bool                                  cancelled = false;
    std::vector<AskUserQuestionAnswer>    answers;
};

class AskUserQuestionPrompter {
public:
    explicit AskUserQuestionPrompter(EventDispatcher& events,
                                       std::chrono::milliseconds timeout = std::chrono::minutes(5))
        : events_(events), timeout_(timeout) {}

    // 阻塞调用。questions_payload 是已经拼好的 nlohmann::json 数组(供前端
    // 渲染 modal),由调用方组装(避免 prompter 与 ask_user_question_tool
    // 数据结构耦合)。abort_flag 非空时周期性检查。
    AskUserQuestionResponse prompt(const nlohmann::json& questions_payload,
                                     const std::atomic<bool>* abort_flag);

    // 由 SessionClient 间接调用。线程安全。未知 request_id = no-op。
    void notify_response(const std::string& request_id,
                          const AskUserQuestionResponse& response);

    // 测试用 / 调试用: 当前 pending 数。
    std::size_t pending_count() const;

private:
    static std::string make_request_id();

    EventDispatcher&         events_;
    std::chrono::milliseconds timeout_;

    struct Pending {
        std::mutex                  mu;
        std::condition_variable     cv;
        bool                        responded = false;
        AskUserQuestionResponse    response;
    };

    mutable std::mutex                                         pending_mu_;
    std::unordered_map<std::string, std::shared_ptr<Pending>>  pending_;
};

} // namespace acecode
