#include "ask_user_question_tool.hpp"

#include "../tui_state.hpp"
#include "../utils/logger.hpp"

#include <ftxui/component/screen_interactive.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <set>
#include <string>

namespace acecode {

namespace {

// 数一个 UTF-8 字符串的 codepoint 数 —— 12 字符上限必须按字符而不是字节算,
// 不然 "授权方式" (12 bytes in UTF-8, 4 chars) 会被误判越界。
std::size_t utf8_codepoint_count(const std::string& s) {
    std::size_t count = 0;
    const unsigned char* p = reinterpret_cast<const unsigned char*>(s.data());
    std::size_t len = s.size();
    for (std::size_t i = 0; i < len;) {
        unsigned char c = p[i];
        int seq = 1;
        if ((c & 0x80) == 0x00) seq = 1;
        else if ((c & 0xE0) == 0xC0) seq = 2;
        else if ((c & 0xF0) == 0xE0) seq = 3;
        else if ((c & 0xF8) == 0xF0) seq = 4;
        else { i++; continue; }
        i += seq;
        count++;
    }
    return count;
}

constexpr int kMaxHeaderChars = 12;
constexpr int kMinQuestions = 1;
constexpr int kMaxQuestions = 4;
constexpr int kMinOptions = 2;
constexpr int kMaxOptions = 4;

// 工具 description —— 对齐 claudecodehaha `ASK_USER_QUESTION_TOOL_PROMPT`
// 原文,删除 ACECode 没有对应概念的 `Plan mode note:` 段。
constexpr const char* kToolDescription =
    "Asks the user multiple choice questions to gather information, clarify "
    "ambiguity, understand preferences, make decisions or offer them choices. "
    "Use this tool when you need to ask the user questions during execution. "
    "This allows you to:\n"
    "1. Gather user preferences or requirements\n"
    "2. Clarify ambiguous instructions\n"
    "3. Get decisions on implementation choices as you work\n"
    "4. Offer choices to the user about what direction to take.\n"
    "\n"
    "Usage notes:\n"
    "- Users will always be able to select \"Other\" to provide custom text input\n"
    "- Use multiSelect: true to allow multiple answers to be selected for a question\n"
    "- If you recommend a specific option, make that the first option in the list "
    "and add \"(Recommended)\" at the end of the label";

} // namespace

std::optional<std::vector<AskQuestion>> validate_ask_user_question_args(
    const std::string& arguments_json, std::string& err) {
    err.clear();
    if (arguments_json.empty()) {
        err = "[Error] AskUserQuestion requires arguments.";
        return std::nullopt;
    }

    nlohmann::json root;
    try {
        root = nlohmann::json::parse(arguments_json);
    } catch (const std::exception& e) {
        err = std::string("[Error] Failed to parse arguments JSON: ") + e.what();
        return std::nullopt;
    }

    if (!root.is_object() || !root.contains("questions") || !root["questions"].is_array()) {
        err = "[Error] `questions` must be an array (length 1-4).";
        return std::nullopt;
    }
    const auto& qs = root["questions"];
    if (qs.size() < kMinQuestions || qs.size() > kMaxQuestions) {
        err = "[Error] `questions` length must be between 1 and 4 (got " +
              std::to_string(qs.size()) + ").";
        return std::nullopt;
    }

    std::vector<AskQuestion> out;
    out.reserve(qs.size());
    std::set<std::string> seen_questions;

    for (std::size_t qi = 0; qi < qs.size(); ++qi) {
        const auto& q = qs[qi];
        if (!q.is_object()) {
            err = "[Error] questions[" + std::to_string(qi) + "] must be an object.";
            return std::nullopt;
        }
        AskQuestion parsed;
        parsed.question = q.value("question", std::string{});
        parsed.header = q.value("header", std::string{});
        parsed.multi_select = q.value("multiSelect", false);

        if (parsed.question.empty()) {
            err = "[Error] questions[" + std::to_string(qi) +
                  "].question must be a non-empty string.";
            return std::nullopt;
        }
        if (parsed.header.empty()) {
            err = "[Error] questions[" + std::to_string(qi) +
                  "].header must be a non-empty string.";
            return std::nullopt;
        }
        if (utf8_codepoint_count(parsed.header) >
            static_cast<std::size_t>(kMaxHeaderChars)) {
            err = "[Error] questions[" + std::to_string(qi) +
                  "].header is too long (max 12 characters).";
            return std::nullopt;
        }

        if (!seen_questions.insert(parsed.question).second) {
            err = "[Error] Question texts must be unique across `questions`.";
            return std::nullopt;
        }

        if (!q.contains("options") || !q["options"].is_array()) {
            err = "[Error] questions[" + std::to_string(qi) +
                  "].options must be an array (length 2-4).";
            return std::nullopt;
        }
        const auto& opts = q["options"];
        if (opts.size() < kMinOptions || opts.size() > kMaxOptions) {
            err = "[Error] questions[" + std::to_string(qi) +
                  "].options length must be between 2 and 4 (got " +
                  std::to_string(opts.size()) + ").";
            return std::nullopt;
        }

        std::set<std::string> seen_labels;
        for (std::size_t oi = 0; oi < opts.size(); ++oi) {
            const auto& o = opts[oi];
            if (!o.is_object()) {
                err = "[Error] questions[" + std::to_string(qi) + "].options[" +
                      std::to_string(oi) + "] must be an object.";
                return std::nullopt;
            }
            AskOption opt;
            opt.label = o.value("label", std::string{});
            opt.description = o.value("description", std::string{});
            if (opt.label.empty()) {
                err = "[Error] questions[" + std::to_string(qi) + "].options[" +
                      std::to_string(oi) + "].label must be non-empty.";
                return std::nullopt;
            }
            if (!seen_labels.insert(opt.label).second) {
                err = "[Error] Option labels must be unique within questions[" +
                      std::to_string(qi) + "].";
                return std::nullopt;
            }
            // preview 字段如果存在,必须是字符串(类型错误早发现),但内容被忽略。
            if (o.contains("preview") && !o["preview"].is_null() && !o["preview"].is_string()) {
                err = "[Error] questions[" + std::to_string(qi) + "].options[" +
                      std::to_string(oi) + "].preview must be a string if present.";
                return std::nullopt;
            }
            parsed.options.push_back(std::move(opt));
        }

        out.push_back(std::move(parsed));
    }

    return out;
}

std::string format_ask_answers(
    const std::vector<std::string>& question_order,
    const std::map<std::string, std::string>& answers) {
    std::string out = "User has answered your questions: ";
    bool first = true;
    for (const auto& q : question_order) {
        auto it = answers.find(q);
        const std::string& a = (it == answers.end()) ? std::string{} : it->second;
        if (!first) out += ", ";
        out += "\"";
        out += q;
        out += "\"=\"";
        out += a;
        out += "\"";
        first = false;
    }
    return out;
}

ToolResult make_rejected_ask_result() {
    ToolResult r;
    r.output = "[Error] User declined to answer questions.";
    r.success = false;
    return r;
}

ToolImpl create_ask_user_question_tool(TuiState& state,
                                        ftxui::ScreenInteractive& screen) {
    ToolDef def;
    def.name = "AskUserQuestion";
    def.description = kToolDescription;

    // JSON Schema —— questions[1..4] 每个含 question/header/options[2..4]/multiSelect。
    // 单一选项含 label/description,可选 preview(实现忽略但允许传入)。
    nlohmann::json option_schema = {
        {"type", "object"},
        {"required", nlohmann::json::array({"label", "description"})},
        {"properties", {
            {"label", {
                {"type", "string"},
                {"description",
                 "Short (1-5 word) label shown to the user as the selectable choice."}
            }},
            {"description", {
                {"type", "string"},
                {"description",
                 "Explanation of what this option means or what will happen if chosen."}
            }},
            {"preview", {
                {"type", "string"},
                {"description",
                 "Optional preview content. Accepted for SDK-schema parity but not "
                 "rendered by the ACECode terminal UI."}
            }}
        }}
    };

    nlohmann::json question_schema = {
        {"type", "object"},
        {"required", nlohmann::json::array({"question", "header", "options"})},
        {"properties", {
            {"question", {
                {"type", "string"},
                {"description",
                 "The complete question. Should be clear, specific and end with '?'."}
            }},
            {"header", {
                {"type", "string"},
                {"description",
                 "Very short chip label (max 12 characters). Examples: 'Library', 'Approach'."}
            }},
            {"options", {
                {"type", "array"},
                {"minItems", kMinOptions},
                {"maxItems", kMaxOptions},
                {"items", option_schema},
                {"description",
                 "2-4 mutually exclusive choices. Do NOT include an 'Other' option — "
                 "the UI appends one automatically."}
            }},
            {"multiSelect", {
                {"type", "boolean"},
                {"default", false},
                {"description", "Set true to allow the user to pick multiple options."}
            }}
        }}
    };

    def.parameters = {
        {"type", "object"},
        {"required", nlohmann::json::array({"questions"})},
        {"properties", {
            {"questions", {
                {"type", "array"},
                {"minItems", kMinQuestions},
                {"maxItems", kMaxQuestions},
                {"items", question_schema},
                {"description",
                 "1-4 questions to ask the user. Question texts must be unique."}
            }}
        }}
    };

    auto execute = [&state, &screen](const std::string& arguments_json,
                                     const ToolContext& ctx) -> ToolResult {
        std::string err;
        auto parsed = validate_ask_user_question_args(arguments_json, err);
        if (!parsed.has_value()) {
            return ToolResult{err, false};
        }

        // 记录 question 的原始顺序,供 format 拼接 + 防止 std::map 里乱序。
        std::vector<std::string> question_order;
        question_order.reserve(parsed->size());
        for (const auto& q : *parsed) question_order.push_back(q.question);

        const std::atomic<bool>* abort = ctx.abort_flag;

        // 若 shutdown 已经启动,直接返回拒绝,不去动 TUI。
        if (abort && abort->load()) {
            return make_rejected_ask_result();
        }

        {
            std::unique_lock<std::mutex> lk(state.mu);
            state.ask_pending = true;
            state.ask_payload_json = arguments_json;
            state.ask_questions = *parsed;
            state.ask_question_order = question_order;
            state.ask_result_answers.clear();
            state.ask_result_ok = false;
            state.ask_current_question = 0;
            state.ask_option_focus = 0;
            state.ask_multi_selected.assign(
                (*parsed)[0].options.size(), false);
            state.ask_other_input_active = false;
        }
        screen.PostEvent(ftxui::Event::Custom);

        std::map<std::string, std::string> answers;
        bool ok = false;
        bool aborted = false;
        {
            std::unique_lock<std::mutex> lk(state.mu);
            state.ask_cv.wait(lk, [&state, abort] {
                return !state.ask_pending || (abort && abort->load());
            });
            aborted = abort && abort->load();
            ok = state.ask_result_ok;
            answers = state.ask_result_answers;
            // overlay 已关闭 —— 清理残留的临时 navigation 状态,防止下次打开时脏数据。
            state.ask_pending = false;
            state.ask_questions.clear();
            state.ask_question_order.clear();
            state.ask_multi_selected.clear();
            state.ask_other_input_active = false;
            state.ask_current_question = 0;
            state.ask_option_focus = 0;
        }
        screen.PostEvent(ftxui::Event::Custom);

        if (aborted || !ok) {
            LOG_INFO("[AskUserQuestion] declined (aborted=" +
                     std::string(aborted ? "true" : "false") + ")");
            return make_rejected_ask_result();
        }

        ToolResult r;
        r.success = true;
        r.output = format_ask_answers(question_order, answers);
        return r;
    };

    ToolImpl impl;
    impl.definition = def;
    impl.execute = execute;
    impl.is_read_only = true;
    impl.source = ToolSource::Builtin;
    return impl;
}

namespace {

// 构造 daemon 工厂会用到的同一份 ToolDef。复用 create_ask_user_question_tool
// 那段拼装太长 —— 把 def 抽出来共享。
ToolDef build_ask_user_question_def() {
    ToolDef def;
    def.name = "AskUserQuestion";
    def.description = kToolDescription;

    nlohmann::json option_schema = {
        {"type", "object"},
        {"required", nlohmann::json::array({"label", "description"})},
        {"properties", {
            {"label", {
                {"type", "string"},
                {"description",
                 "Short (1-5 word) label shown to the user as the selectable choice."}
            }},
            {"description", {
                {"type", "string"},
                {"description",
                 "Explanation of what this option means or what will happen if chosen."}
            }},
            {"preview", {
                {"type", "string"},
                {"description",
                 "Optional preview content. Accepted for SDK-schema parity."}
            }}
        }}
    };

    nlohmann::json question_schema = {
        {"type", "object"},
        {"required", nlohmann::json::array({"question", "header", "options"})},
        {"properties", {
            {"question", {
                {"type", "string"},
                {"description",
                 "The complete question. Should be clear, specific and end with '?'."}
            }},
            {"header", {
                {"type", "string"},
                {"description",
                 "Very short chip label (max 12 characters)."}
            }},
            {"options", {
                {"type", "array"},
                {"minItems", kMinOptions},
                {"maxItems", kMaxOptions},
                {"items", option_schema},
                {"description",
                 "2-4 mutually exclusive choices. Do NOT include an 'Other' option — "
                 "the UI appends one automatically."}
            }},
            {"multiSelect", {
                {"type", "boolean"},
                {"default", false},
                {"description", "Set true to allow the user to pick multiple options."}
            }}
        }}
    };

    def.parameters = {
        {"type", "object"},
        {"required", nlohmann::json::array({"questions"})},
        {"properties", {
            {"questions", {
                {"type", "array"},
                {"minItems", kMinQuestions},
                {"maxItems", kMaxQuestions},
                {"items", question_schema},
                {"description",
                 "1-4 questions to ask the user. Question texts must be unique."}
            }}
        }}
    };
    return def;
}

// 把已 validate 的 question 列表转成 prompter 用的 questions_payload(给前端渲染)。
// 字段名与 design.md / spec.md 的 WS 协议对齐:每个 question 携带 id(用 question
// 文本作为 id,与 TUI 行为同步) / text / options[{label,value}] / multiSelect。
nlohmann::json questions_to_payload(const std::vector<AskQuestion>& qs) {
    nlohmann::json arr = nlohmann::json::array();
    for (const auto& q : qs) {
        nlohmann::json options = nlohmann::json::array();
        for (const auto& o : q.options) {
            options.push_back({
                {"label", o.label},
                {"value", o.label}, // value=label,前端 v1 不区分两者
                {"description", o.description},
            });
        }
        arr.push_back({
            {"id",          q.question},
            {"text",        q.question},
            {"header",      q.header},
            {"options",     options},
            {"multiSelect", q.multi_select},
        });
    }
    return arr;
}

// 把 ctx.ask_user_questions 回来的 JSON 转成 std::map<question, answer_text>,
// 按 ", " 拼合 multiSelect。供 format_ask_answers 使用。
std::map<std::string, std::string>
parse_async_response(const nlohmann::json& resp_json) {
    std::map<std::string, std::string> answers;
    if (!resp_json.is_object()) return answers;
    if (!resp_json.contains("answers") || !resp_json["answers"].is_array()) return answers;
    for (const auto& a : resp_json["answers"]) {
        if (!a.is_object()) continue;
        std::string qid = a.value("question_id", std::string{});
        if (qid.empty()) continue;

        // selected 与 custom_text 都可能存在 —— 拼合
        std::vector<std::string> parts;
        if (a.contains("selected") && a["selected"].is_array()) {
            for (const auto& s : a["selected"]) {
                if (s.is_string()) parts.push_back(s.get<std::string>());
            }
        }
        if (a.contains("custom_text") && a["custom_text"].is_string()) {
            std::string ct = a["custom_text"].get<std::string>();
            if (!ct.empty()) parts.push_back(ct);
        }

        std::string joined;
        for (std::size_t i = 0; i < parts.size(); ++i) {
            if (i) joined += ", ";
            joined += parts[i];
        }
        answers[qid] = joined;
    }
    return answers;
}

} // namespace

ToolImpl create_ask_user_question_tool_async() {
    auto execute = [](const std::string& arguments_json,
                       const ToolContext& ctx) -> ToolResult {
        std::string err;
        auto parsed = validate_ask_user_question_args(arguments_json, err);
        if (!parsed.has_value()) {
            return ToolResult{err, false};
        }

        // ctx.ask_user_questions 为空 → daemon 没装 prompter,工具不可用。
        // 直接拒绝 + 让 LLM 知道(避免无限挂起)。
        if (!ctx.ask_user_questions) {
            return ToolResult{
                "[Error] AskUserQuestion is not supported by this session "
                "(no UI channel connected).",
                false};
        }

        // abort 已触发 = 不发问,直接 reject
        if (ctx.abort_flag && ctx.abort_flag->load()) {
            return make_rejected_ask_result();
        }

        std::vector<std::string> question_order;
        question_order.reserve(parsed->size());
        for (const auto& q : *parsed) question_order.push_back(q.question);

        nlohmann::json payload = questions_to_payload(*parsed);
        nlohmann::json resp = ctx.ask_user_questions(payload);

        bool cancelled = resp.value("cancelled", false);
        if (cancelled) {
            return make_rejected_ask_result();
        }

        auto answers = parse_async_response(resp);
        ToolResult r;
        r.success = true;
        r.output  = format_ask_answers(question_order, answers);
        return r;
    };

    ToolImpl impl;
    impl.definition   = build_ask_user_question_def();
    impl.execute      = execute;
    impl.is_read_only = true;
    impl.source       = ToolSource::Builtin;
    return impl;
}

} // namespace acecode
