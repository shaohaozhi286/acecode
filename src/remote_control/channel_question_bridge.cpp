#include "channel_question_bridge.hpp"

#include <algorithm>
#include <cctype>
#include <limits>
#include <set>
#include <sstream>
#include <utility>

namespace acecode::rc {

namespace {

constexpr std::size_t kMaxCustomAnswerCodepoints = 2000;
constexpr std::size_t kClosedRequestHistoryLimit = 256;

std::string trim_ascii(const std::string& text) {
    std::size_t begin = 0;
    while (begin < text.size() &&
           std::isspace(static_cast<unsigned char>(text[begin]))) {
        ++begin;
    }
    std::size_t end = text.size();
    while (end > begin &&
           std::isspace(static_cast<unsigned char>(text[end - 1]))) {
        --end;
    }
    return text.substr(begin, end - begin);
}

bool starts_with(const std::string& text, const std::string& prefix) {
    return text.size() >= prefix.size() &&
           text.compare(0, prefix.size(), prefix) == 0;
}

void set_error(std::string* error, std::string message) {
    if (error) *error = std::move(message);
}

std::optional<std::size_t> utf8_codepoint_count(const std::string& text) {
    std::size_t count = 0;
    for (std::size_t i = 0; i < text.size();) {
        const auto lead = static_cast<unsigned char>(text[i]);
        std::size_t width = 0;
        if (lead <= 0x7F) {
            width = 1;
        } else if (lead >= 0xC2 && lead <= 0xDF) {
            width = 2;
        } else if (lead >= 0xE0 && lead <= 0xEF) {
            width = 3;
        } else if (lead >= 0xF0 && lead <= 0xF4) {
            width = 4;
        } else {
            return std::nullopt;
        }
        if (i + width > text.size()) return std::nullopt;
        for (std::size_t j = 1; j < width; ++j) {
            const auto continuation = static_cast<unsigned char>(text[i + j]);
            if ((continuation & 0xC0) != 0x80) return std::nullopt;
        }
        if (width == 3) {
            const auto second = static_cast<unsigned char>(text[i + 1]);
            if ((lead == 0xE0 && second < 0xA0) ||
                (lead == 0xED && second >= 0xA0)) {
                return std::nullopt;
            }
        }
        if (width == 4) {
            const auto second = static_cast<unsigned char>(text[i + 1]);
            if ((lead == 0xF0 && second < 0x90) ||
                (lead == 0xF4 && second >= 0x90)) {
                return std::nullopt;
            }
        }
        i += width;
        ++count;
    }
    return count;
}

std::int64_t remaining_seconds(
    const std::optional<ChannelQuestionBridge::Clock::time_point>& deadline,
    ChannelQuestionBridge::Clock::time_point now) {
    if (!deadline.has_value() || now >= *deadline) return 0;
    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
        *deadline - now);
    return (remaining.count() + 999) / 1000;
}

std::string usage_text() {
    return "用法：/aq <序号|选项标签|自定义答案>，或使用 "
           "/aq --repeat、/aq --back、/aq --status、/aq --cancel。";
}

std::string normalize_wide_commas(const std::string& input) {
    std::string normalized;
    normalized.reserve(input.size());
    for (std::size_t i = 0; i < input.size();) {
        if (i + 2 < input.size() &&
            static_cast<unsigned char>(input[i]) == 0xEF &&
            static_cast<unsigned char>(input[i + 1]) == 0xBC &&
            static_cast<unsigned char>(input[i + 2]) == 0x8C) {
            normalized.push_back(',');
            i += 3;
        } else {
            normalized.push_back(input[i]);
            ++i;
        }
    }
    return normalized;
}

bool all_ascii_digits(const std::string& text) {
    if (text.empty()) return false;
    return std::all_of(text.begin(), text.end(), [](unsigned char ch) {
        return ch >= '0' && ch <= '9';
    });
}

bool tokenize_numeric_selection(const std::string& input,
                                std::vector<std::string>* tokens,
                                std::string* error) {
    const std::string normalized = normalize_wide_commas(input);
    const bool has_comma = normalized.find(',') != std::string::npos;
    const bool begins_numeric_list =
        !normalized.empty() &&
        ((normalized.front() >= '0' && normalized.front() <= '9') ||
         normalized.front() == ',');

    std::vector<std::string> whitespace_tokens;
    {
        std::istringstream stream(normalized);
        std::string token;
        while (stream >> token) whitespace_tokens.push_back(std::move(token));
    }

    bool whitespace_numeric_list = whitespace_tokens.size() > 1;
    if (whitespace_numeric_list) {
        for (const auto& token : whitespace_tokens) {
            if (!all_ascii_digits(token)) {
                whitespace_numeric_list = false;
                break;
            }
        }
    }
    const bool pure_number = all_ascii_digits(normalized);
    if (!(has_comma && begins_numeric_list) &&
        !whitespace_numeric_list &&
        !pure_number) {
        return false;
    }

    std::string current;
    bool comma_waiting_for_value = false;
    for (unsigned char ch : normalized) {
        if (ch >= '0' && ch <= '9') {
            current.push_back(static_cast<char>(ch));
            comma_waiting_for_value = false;
            continue;
        }
        if (ch == ',') {
            if (comma_waiting_for_value) {
                set_error(error, "多选序号格式无效。");
                return true;
            }
            if (!current.empty()) {
                tokens->push_back(std::move(current));
                current.clear();
            } else if (tokens->empty()) {
                set_error(error, "多选序号格式无效。");
                return true;
            }
            comma_waiting_for_value = true;
            continue;
        }
        if (std::isspace(ch)) {
            if (!current.empty()) {
                tokens->push_back(std::move(current));
                current.clear();
            }
            continue;
        }
        set_error(error, "多选只接受逗号、中文逗号或空格分隔的序号。");
        return true;
    }
    if (comma_waiting_for_value || (current.empty() && tokens->empty())) {
        set_error(error, "多选序号格式无效。");
        return true;
    }
    if (!current.empty()) tokens->push_back(std::move(current));
    return true;
}

} // namespace

bool ChannelQuestionBridge::is_control_input(const std::string& text) {
    const auto trimmed = trim_ascii(text);
    return trimmed == "/aq" || starts_with(trimmed, "/aq ");
}

std::optional<PendingQuestionRequestSnapshot>
ChannelQuestionBridge::request_from_event(
    const nlohmann::json& payload,
    std::uint64_t fallback_order,
    Clock::time_point observed_at,
    std::string* error) {
    if (!payload.is_object()) {
        set_error(error, "QuestionRequest payload 必须是 object。");
        return std::nullopt;
    }
    if (!payload.contains("request_id") || !payload["request_id"].is_string() ||
        payload["request_id"].get<std::string>().empty()) {
        set_error(error, "QuestionRequest 缺少 request_id。");
        return std::nullopt;
    }
    if (!payload.contains("questions") || !payload["questions"].is_array()) {
        set_error(error, "QuestionRequest 缺少 questions array。");
        return std::nullopt;
    }

    PendingQuestionRequestSnapshot request;
    request.request_id = payload["request_id"].get<std::string>();
    request.questions = payload["questions"];
    request.created_at = observed_at;
    request.order = fallback_order;
    if (payload.contains("request_order")) {
        if (!payload["request_order"].is_number_unsigned()) {
            set_error(error, "QuestionRequest request_order 类型无效。");
            return std::nullopt;
        }
        request.order = payload["request_order"].get<std::uint64_t>();
    }
    if (request.order == 0) {
        set_error(error, "QuestionRequest 创建顺序无效。");
        return std::nullopt;
    }

    if (payload.contains("timeout_ms")) {
        if (!payload["timeout_ms"].is_number_integer()) {
            set_error(error, "QuestionRequest timeout_ms 类型无效。");
            return std::nullopt;
        }
        const auto timeout_ms = payload["timeout_ms"].get<std::int64_t>();
        if (timeout_ms < 0) {
            set_error(error, "QuestionRequest timeout_ms 不能为负数。");
            return std::nullopt;
        }
        if (timeout_ms > 0) {
            request.deadline = observed_at + std::chrono::milliseconds(timeout_ms);
        }
    }
    return request;
}

std::optional<std::vector<ChannelQuestionBridge::Question>>
ChannelQuestionBridge::parse_questions(const nlohmann::json& questions,
                                       std::string* error) {
    if (!questions.is_array() || questions.empty()) {
        set_error(error, "questions 必须是非空 array。");
        return std::nullopt;
    }

    std::vector<Question> parsed;
    parsed.reserve(questions.size());
    for (std::size_t question_index = 0;
         question_index < questions.size();
         ++question_index) {
        const auto& raw_question = questions[question_index];
        if (!raw_question.is_object()) {
            set_error(error, "question 必须是 object。");
            return std::nullopt;
        }
        for (const auto* field : {"id", "text", "header"}) {
            if (!raw_question.contains(field) ||
                !raw_question[field].is_string() ||
                raw_question[field].get<std::string>().empty()) {
                set_error(error, std::string("question 缺少 ") + field + "。");
                return std::nullopt;
            }
        }
        if (!raw_question.contains("options") ||
            !raw_question["options"].is_array() ||
            raw_question["options"].empty()) {
            set_error(error, "question 缺少非空 options array。");
            return std::nullopt;
        }
        if (raw_question.contains("multiSelect") &&
            !raw_question["multiSelect"].is_boolean()) {
            set_error(error, "question multiSelect 类型无效。");
            return std::nullopt;
        }

        Question question;
        question.id = raw_question["id"].get<std::string>();
        question.text = raw_question["text"].get<std::string>();
        question.header = raw_question["header"].get<std::string>();
        question.multi_select = raw_question.value("multiSelect", false);

        std::set<std::string> labels;
        for (const auto& raw_option : raw_question["options"]) {
            if (!raw_option.is_object() ||
                !raw_option.contains("label") ||
                !raw_option["label"].is_string() ||
                raw_option["label"].get<std::string>().empty()) {
                set_error(error, "option 缺少非空 label。");
                return std::nullopt;
            }
            if (raw_option.contains("description") &&
                !raw_option["description"].is_string()) {
                set_error(error, "option description 类型无效。");
                return std::nullopt;
            }
            Option option;
            option.label = raw_option["label"].get<std::string>();
            option.description =
                raw_option.value("description", std::string{});
            if (!labels.insert(option.label).second) {
                set_error(error, "option label 不能重复。");
                return std::nullopt;
            }
            question.options.push_back(std::move(option));
        }
        parsed.push_back(std::move(question));
    }
    return parsed;
}

bool ChannelQuestionBridge::parse_answer(const Question& question,
                                         const std::string& input,
                                         AskUserQuestionAnswer* answer,
                                         std::string* error) {
    std::vector<std::string> numeric_tokens;
    std::string numeric_error;
    const bool numeric_attempt =
        tokenize_numeric_selection(input, &numeric_tokens, &numeric_error);
    if (numeric_attempt) {
        if (!numeric_error.empty()) {
            set_error(error, std::move(numeric_error));
            return false;
        }
        if (!question.multi_select && numeric_tokens.size() > 1) {
            set_error(error, "本题为单选，只能提交一个序号。");
            return false;
        }

        std::set<std::size_t> seen;
        std::vector<std::string> selected;
        selected.reserve(numeric_tokens.size());
        for (const auto& token : numeric_tokens) {
            std::size_t index = 0;
            try {
                const auto parsed = std::stoull(token);
                if (parsed > std::numeric_limits<std::size_t>::max()) {
                    set_error(error, "序号无效。");
                    return false;
                }
                index = static_cast<std::size_t>(parsed);
            } catch (...) {
                set_error(error, "序号无效。");
                return false;
            }
            if (index == 0 || index > question.options.size()) {
                set_error(error, "序号超出有效选项范围。");
                return false;
            }
            if (!seen.insert(index).second) {
                set_error(error, "多选序号不能重复。");
                return false;
            }
            selected.push_back(question.options[index - 1].label);
        }
        answer->question_id = question.id;
        answer->selected = std::move(selected);
        answer->custom_text.clear();
        return true;
    }

    for (const auto& option : question.options) {
        if (input == option.label) {
            answer->question_id = question.id;
            answer->selected = {option.label};
            answer->custom_text.clear();
            return true;
        }
    }

    const auto codepoints = utf8_codepoint_count(input);
    if (!codepoints.has_value()) {
        set_error(error, "自定义答案不是有效的 UTF-8 文本。");
        return false;
    }
    if (*codepoints > kMaxCustomAnswerCodepoints) {
        set_error(error, "自定义答案最多 2000 个 Unicode 码点。");
        return false;
    }
    answer->question_id = question.id;
    answer->selected.clear();
    answer->custom_text = input;
    return true;
}

bool ChannelQuestionBridge::has_request_locked(
    const std::string& request_id) const {
    if (closed_requests_.find(request_id) != closed_requests_.end()) return true;
    return std::any_of(
        batches_.begin(), batches_.end(), [&](const Batch& batch) {
            return batch.request.request_id == request_id;
        });
}

void ChannelQuestionBridge::remember_closed_locked(
    const std::string& request_id,
    const std::string& reason) {
    if (request_id.empty()) return;
    const auto [it, inserted] =
        closed_requests_.insert_or_assign(request_id, reason);
    (void)it;
    if (!inserted) return;

    closed_request_order_.push_back(request_id);
    while (closed_request_order_.size() > kClosedRequestHistoryLimit) {
        closed_requests_.erase(closed_request_order_.front());
        closed_request_order_.pop_front();
    }
}

bool ChannelQuestionBridge::insert_request_locked(
    PendingQuestionRequestSnapshot request,
    std::string* error) {
    if (request.request_id.empty()) {
        set_error(error, "request_id 不能为空。");
        return false;
    }
    if (request.order == 0) {
        set_error(error, "request order 必须大于 0。");
        return false;
    }
    if (has_request_locked(request.request_id)) return false;
    auto questions = parse_questions(request.questions, error);
    if (!questions.has_value()) return false;

    Batch batch;
    batch.request = std::move(request);
    batch.questions = std::move(*questions);
    batch.answers.resize(batch.questions.size());

    auto position = std::upper_bound(
        batches_.begin(), batches_.end(), batch.request.order,
        [](std::uint64_t order, const Batch& queued) {
            return order < queued.request.order;
        });
    batches_.insert(position, std::move(batch));
    last_close_was_timeout_ = false;
    return true;
}

ChannelQuestionAction ChannelQuestionBridge::render_current_locked(
    Clock::time_point now) const {
    ChannelQuestionAction action;
    if (batches_.empty()) return action;
    const auto& batch = batches_.front();
    if (batch.request.deadline.has_value() &&
        now >= *batch.request.deadline) {
        action.outbound_texts.push_back("问题已超时或已结束");
        return action;
    }

    const auto& question = batch.questions[batch.current_question];
    std::ostringstream out;
    out << "需要你确认（第 " << (batch.current_question + 1) << "/"
        << batch.questions.size() << " 题）【" << question.header << "】\n\n"
        << question.text << "\n";
    for (std::size_t i = 0; i < question.options.size(); ++i) {
        out << "\n" << (i + 1) << ". " << question.options[i].label;
        if (!question.options[i].description.empty()) {
            out << "\n   " << question.options[i].description;
        }
        out << "\n";
    }
    out << "\n请回复“/aq 1”，或回复“/aq 自定义答案”。\n"
        << "多选题可回复“/aq 1,3”。\n"
        << "也可以直接在 ACECode 页面完成。";
    if (batch.request.deadline.has_value()) {
        out << "\n本批问题将在 "
            << remaining_seconds(batch.request.deadline, now)
            << " 秒后超时。";
    }
    action.outbound_texts.push_back(out.str());
    return action;
}

ChannelQuestionAction ChannelQuestionBridge::add_request(
    PendingQuestionRequestSnapshot request,
    Clock::time_point now) {
    std::lock_guard<std::mutex> lock(mu_);
    const std::string request_id = request.request_id;
    const bool existed = has_request_locked(request_id);
    std::string error;
    if (!insert_request_locked(std::move(request), &error)) {
        ChannelQuestionAction action;
        if (!existed && !error.empty()) {
            action.outbound_texts.push_back(
                "无法展示待回答问题：" + error);
        }
        return action;
    }
    if (!batches_.empty() &&
        batches_.front().request.request_id == request_id) {
        return render_current_locked(now);
    }
    return {};
}

ChannelQuestionAction ChannelQuestionBridge::merge_snapshot(
    std::vector<PendingQuestionRequestSnapshot> requests,
    Clock::time_point now) {
    std::sort(
        requests.begin(), requests.end(),
        [](const auto& lhs, const auto& rhs) {
            return lhs.order < rhs.order;
        });
    std::lock_guard<std::mutex> lock(mu_);
    ChannelQuestionAction action;
    for (auto& request : requests) {
        const bool existed = has_request_locked(request.request_id);
        std::string error;
        if (!insert_request_locked(std::move(request), &error) &&
            !existed && !error.empty()) {
            action.outbound_texts.push_back(
                "无法展示待回答问题：" + error);
        }
    }
    auto prompt = render_current_locked(now);
    action.outbound_texts.insert(
        action.outbound_texts.end(),
        std::make_move_iterator(prompt.outbound_texts.begin()),
        std::make_move_iterator(prompt.outbound_texts.end()));
    return action;
}

ChannelQuestionAction ChannelQuestionBridge::announce_current(
    Clock::time_point now) const {
    std::lock_guard<std::mutex> lock(mu_);
    return render_current_locked(now);
}

ChannelQuestionAction ChannelQuestionBridge::handle_input(
    const std::string& text,
    Clock::time_point now) {
    ChannelQuestionAction action;
    if (!is_control_input(text)) return action;
    action.handled = true;

    const auto trimmed = trim_ascii(text);
    const std::string input =
        trimmed == "/aq" ? std::string{} : trim_ascii(trimmed.substr(4));

    std::lock_guard<std::mutex> lock(mu_);
    if (batches_.empty()) {
        if (last_close_was_timeout_) {
            action.outbound_texts.push_back("问题已超时或已结束");
            last_close_was_timeout_ = false;
        } else {
            action.outbound_texts.push_back("当前没有待回答的问题");
        }
        return action;
    }

    auto& batch = batches_.front();
    if (batch.request.deadline.has_value() &&
        now >= *batch.request.deadline) {
        action.outbound_texts.push_back("问题已超时或已结束");
        return action;
    }
    if (batch.submission_phase != SubmissionPhase::None) {
        action.outbound_texts.push_back("答案已提交，等待问题关闭");
        return action;
    }

    if (input.empty()) {
        action.outbound_texts.push_back(usage_text());
        return action;
    }
    if (starts_with(input, "--")) {
        if (input == "--repeat") {
            auto prompt = render_current_locked(now);
            action.outbound_texts = std::move(prompt.outbound_texts);
            return action;
        }
        if (input == "--back") {
            if (batch.current_question == 0) {
                action.outbound_texts.push_back("已经是第一题，无法继续返回。");
                return action;
            }
            --batch.current_question;
            auto prompt = render_current_locked(now);
            action.outbound_texts = std::move(prompt.outbound_texts);
            return action;
        }
        if (input == "--status") {
            const auto answered = static_cast<std::size_t>(std::count_if(
                batch.answers.begin(), batch.answers.end(),
                [](const auto& answer) { return answer.has_value(); }));
            std::ostringstream status;
            status << "当前第 " << (batch.current_question + 1) << "/"
                   << batch.questions.size() << " 题；已记录 "
                   << answered << "/" << batch.questions.size()
                   << " 题；队列共 " << batches_.size() << " 批。";
            if (batch.request.deadline.has_value()) {
                status << "\n本批剩余 "
                       << remaining_seconds(batch.request.deadline, now)
                       << " 秒超时。";
            }
            action.outbound_texts.push_back(status.str());
            return action;
        }
        if (input == "--cancel") {
            batch.submission_phase = SubmissionPhase::Submitting;
            batch.submitted_cancelled = true;
            ChannelQuestionSubmission submission;
            submission.request_id = batch.request.request_id;
            submission.response.cancelled = true;
            action.submission = std::move(submission);
            return action;
        }
        action.outbound_texts.push_back(usage_text());
        return action;
    }

    AskUserQuestionAnswer answer;
    std::string error;
    const auto question_index = batch.current_question;
    if (!parse_answer(batch.questions[question_index], input, &answer, &error)) {
        action.outbound_texts.push_back("回答无效：" + error);
        return action;
    }

    batch.answers[question_index] = std::move(answer);
    action.outbound_texts.push_back(
        "已记录第 " + std::to_string(question_index + 1) + "/" +
        std::to_string(batch.questions.size()) + " 题");

    if (question_index + 1 < batch.questions.size()) {
        ++batch.current_question;
        auto prompt = render_current_locked(now);
        action.outbound_texts.insert(
            action.outbound_texts.end(),
            std::make_move_iterator(prompt.outbound_texts.begin()),
            std::make_move_iterator(prompt.outbound_texts.end()));
        return action;
    }

    ChannelQuestionSubmission submission;
    submission.request_id = batch.request.request_id;
    submission.response.cancelled = false;
    submission.response.answers.reserve(batch.answers.size());
    for (const auto& draft : batch.answers) {
        if (!draft.has_value()) {
            action.outbound_texts.push_back(
                "回答状态不完整，请使用 /aq --status 检查。");
            return action;
        }
        submission.response.answers.push_back(*draft);
    }
    batch.submission_phase = SubmissionPhase::Submitting;
    batch.submitted_cancelled = false;
    action.submission = std::move(submission);
    return action;
}

ChannelQuestionAction ChannelQuestionBridge::finalize_close_locked(
    std::size_t index,
    const std::string& reason,
    Clock::time_point now) {
    ChannelQuestionAction action;
    if (index >= batches_.size()) return action;

    const bool was_front = index == 0;
    const auto phase = batches_[index].submission_phase;
    const auto request_id = batches_[index].request.request_id;
    remember_closed_locked(request_id, reason);
    batches_.erase(batches_.begin() + static_cast<std::ptrdiff_t>(index));

    if (reason == "timeout") last_close_was_timeout_ = true;
    if (was_front && phase != SubmissionPhase::Accepted) {
        if (reason == "answered") {
            action.outbound_texts.push_back(
                "问题已在 ACECode 页面完成，本端草稿已清除");
        } else if (reason == "timeout") {
            action.outbound_texts.push_back("问题已超时，本端草稿已清除");
        } else {
            action.outbound_texts.push_back("问题已结束，本端草稿已清除");
        }
    }
    if (was_front && !batches_.empty()) {
        last_close_was_timeout_ = false;
        auto prompt = render_current_locked(now);
        action.outbound_texts.insert(
            action.outbound_texts.end(),
            std::make_move_iterator(prompt.outbound_texts.begin()),
            std::make_move_iterator(prompt.outbound_texts.end()));
    }
    return action;
}

ChannelQuestionAction ChannelQuestionBridge::close_request(
    const std::string& request_id,
    const std::string& reason,
    Clock::time_point now) {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = std::find_if(
        batches_.begin(), batches_.end(), [&](const Batch& batch) {
            return batch.request.request_id == request_id;
        });
    if (it == batches_.end()) {
        remember_closed_locked(request_id, reason);
        if (reason == "timeout") last_close_was_timeout_ = true;
        return {};
    }

    if (it->submission_phase == SubmissionPhase::Submitting) {
        it->deferred_close_reason = reason;
        return {};
    }
    const auto index =
        static_cast<std::size_t>(std::distance(batches_.begin(), it));
    return finalize_close_locked(index, reason, now);
}

ChannelQuestionAction ChannelQuestionBridge::complete_submission(
    const std::string& request_id,
    QuestionResponseStatus status,
    Clock::time_point now) {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = std::find_if(
        batches_.begin(), batches_.end(), [&](const Batch& batch) {
            return batch.request.request_id == request_id;
        });
    if (it == batches_.end() ||
        it->submission_phase != SubmissionPhase::Submitting) {
        return {};
    }

    it->submission_phase =
        status == QuestionResponseStatus::Accepted
            ? SubmissionPhase::Accepted
            : SubmissionPhase::Rejected;
    ChannelQuestionAction action;
    if (status == QuestionResponseStatus::Accepted) {
        action.outbound_texts.push_back(
            it->submitted_cancelled
                ? "已取消当前整批问题"
                : "答案已提交，继续执行");
    }
    if (!it->deferred_close_reason.has_value()) return action;

    const auto index =
        static_cast<std::size_t>(std::distance(batches_.begin(), it));
    const auto reason = *it->deferred_close_reason;
    auto closed = finalize_close_locked(index, reason, now);
    action.outbound_texts.insert(
        action.outbound_texts.end(),
        std::make_move_iterator(closed.outbound_texts.begin()),
        std::make_move_iterator(closed.outbound_texts.end()));
    return action;
}

std::size_t ChannelQuestionBridge::pending_count() const {
    std::lock_guard<std::mutex> lock(mu_);
    return batches_.size();
}

std::string ChannelQuestionBridge::current_request_id() const {
    std::lock_guard<std::mutex> lock(mu_);
    return batches_.empty() ? std::string{}
                            : batches_.front().request.request_id;
}

} // namespace acecode::rc
