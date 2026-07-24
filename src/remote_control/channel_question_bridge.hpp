#pragma once

#include "../session/ask_user_question_prompter.hpp"
#include "../session/session_client.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace acecode::rc {

struct ChannelQuestionSubmission {
    std::string request_id;
    AskUserQuestionResponse response;
};

// 纯状态机动作。handled 只对 handle_input 有意义；outbound_texts 由 binder
// 按顺序进入 assistant_message FIFO；submission 必须在状态机锁外提交。
struct ChannelQuestionAction {
    bool handled = false;
    std::vector<std::string> outbound_texts;
    std::optional<ChannelQuestionSubmission> submission;
};

// AskUserQuestion 在通用纯文本 channel 上的内存投影。它不写 transcript、
// 不持有 SessionClient/Hub，也不创建自己的 timeout；QuestionClosed 是唯一
// 移除批次的权威信号。
class ChannelQuestionBridge {
public:
    using Clock = PendingQuestionRequestSnapshot::Clock;

    static bool is_control_input(const std::string& text);

    // 把实时 QuestionRequest payload 转成强类型请求。fallback_order 通常使用
    // SessionEvent::seq；新 prompter payload 自带 request_order 时优先使用它。
    static std::optional<PendingQuestionRequestSnapshot> request_from_event(
        const nlohmann::json& payload,
        std::uint64_t fallback_order,
        Clock::time_point observed_at,
        std::string* error = nullptr);

    ChannelQuestionAction add_request(
        PendingQuestionRequestSnapshot request,
        Clock::time_point now = Clock::now());

    // 订阅建立后的快照合并。按 request.order 排序、与实时帧去重，最后只
    // 展示 FIFO 队首一次。
    ChannelQuestionAction merge_snapshot(
        std::vector<PendingQuestionRequestSnapshot> requests,
        Clock::time_point now = Clock::now());

    ChannelQuestionAction announce_current(
        Clock::time_point now = Clock::now()) const;

    ChannelQuestionAction handle_input(
        const std::string& text,
        Clock::time_point now = Clock::now());

    ChannelQuestionAction close_request(
        const std::string& request_id,
        const std::string& reason,
        Clock::time_point now = Clock::now());

    // respond_question 可能同步触发 QuestionClosed；close_request 会先暂存
    // close reason，直到这里拿到 first-wins 结果再决定 Web/channel 文案。
    ChannelQuestionAction complete_submission(
        const std::string& request_id,
        QuestionResponseStatus status,
        Clock::time_point now = Clock::now());

    std::size_t pending_count() const;
    std::string current_request_id() const;

private:
    struct Option {
        std::string label;
        std::string description;
    };

    struct Question {
        std::string id;
        std::string text;
        std::string header;
        bool multi_select = false;
        std::vector<Option> options;
    };

    enum class SubmissionPhase {
        None,
        Submitting,
        Accepted,
        Rejected,
    };

    struct Batch {
        PendingQuestionRequestSnapshot request;
        std::vector<Question> questions;
        std::vector<std::optional<AskUserQuestionAnswer>> answers;
        std::size_t current_question = 0;
        SubmissionPhase submission_phase = SubmissionPhase::None;
        bool submitted_cancelled = false;
        std::optional<std::string> deferred_close_reason;
    };

    static std::optional<std::vector<Question>> parse_questions(
        const nlohmann::json& questions,
        std::string* error);
    static bool parse_answer(const Question& question,
                             const std::string& input,
                             AskUserQuestionAnswer* answer,
                             std::string* error);

    bool has_request_locked(const std::string& request_id) const;
    bool insert_request_locked(PendingQuestionRequestSnapshot request,
                               std::string* error);
    void remember_closed_locked(const std::string& request_id,
                                const std::string& reason);
    ChannelQuestionAction render_current_locked(Clock::time_point now) const;
    ChannelQuestionAction finalize_close_locked(std::size_t index,
                                                const std::string& reason,
                                                Clock::time_point now);

    mutable std::mutex mu_;
    std::vector<Batch> batches_;
    std::unordered_map<std::string, std::string> closed_requests_;
    std::deque<std::string> closed_request_order_;
    bool last_close_was_timeout_ = false;
};

} // namespace acecode::rc
