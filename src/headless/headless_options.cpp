#include "headless_options.hpp"

#include <cctype>
#include <regex>

namespace acecode::headless {

namespace {

bool is_print_flag(const std::string& t) {
    return t == "-p" || t == "--print";
}

bool is_dangerous_flag(const std::string& t) {
    // 与 src/cli/interactive_options.cpp 的别名集合保持一致。
    return t == "-dangerous" || t == "--dangerous" ||
           t == "-yolo" || t == "--yolo";
}

bool valid_permission_mode(const std::string& m) {
    return m == "default" || m == "accept-edits" || m == "plan" || m == "yolo";
}

// "--flag=value" 形式的拆分。命中前缀返回 true 并写 value(可为空串,由
// 调用方判空报错)。
bool split_eq(const std::string& t, const std::string& flag, std::string& value) {
    const std::string prefix = flag + "=";
    if (t.rfind(prefix, 0) != 0) return false;
    value = t.substr(prefix.size());
    return true;
}

// 形如 <YYYYMMDD-HHMMSS-XXXX>-<digits> 的 id 与旧 PID 后缀实验数据的文件名
// 无法区分 —— SessionStorage::list_sessions 会把这种文件名当 legacy 数据
// 隐藏,创建出来的会话在所有列表里隐身,所以 --session-id 直接拒绝。
bool looks_like_legacy_pid_session_id(const std::string& id) {
    static const std::regex re(R"(^\d{8}-\d{6}-[0-9a-f]{4}-\d+$)");
    return std::regex_match(id, re);
}

} // namespace

bool is_valid_session_id_token(const std::string& id) {
    if (id.empty() || id.size() > 64) return false;
    for (char c : id) {
        const auto uc = static_cast<unsigned char>(c);
        if (!std::isalnum(uc) && c != '-' && c != '_') return false;
    }
    return true;
}

bool should_enter_print_mode(const std::vector<std::string>& tokens) {
    if (tokens.empty()) return false;
    // 防御性排除:已有子命令优先(daemon/service/upgrade 在 dispatch 更早分支
    // 处理;configure 在 TUI 前置命令路径处理)。
    const std::string& first = tokens.front();
    if (first == "configure" || first == "daemon" || first == "service" ||
        first == "upgrade" || first == "update") {
        return false;
    }
    for (const auto& t : tokens) {
        if (is_print_flag(t)) return true;
    }
    return false;
}

HeadlessCliOptions parse_headless_cli_options(const std::vector<std::string>& tokens) {
    HeadlessCliOptions o;
    auto fail = [&o](const std::string& msg) -> HeadlessCliOptions& {
        o.error = msg;
        return o;
    };

    // 解析 --max-turns 的值。失败返回 false(调用方 fail 报错)。
    auto parse_max_turns = [&o](const std::string& raw) -> bool {
        try {
            std::size_t pos = 0;
            int n = std::stoi(raw, &pos);
            if (pos != raw.size() || n <= 0) return false;
            o.max_turns = n;
            return true;
        } catch (...) {
            return false;
        }
    };

    for (std::size_t i = 0; i < tokens.size(); ++i) {
        const std::string& t = tokens[i];
        std::string eq_value;

        if (is_print_flag(t)) {
            o.print_mode = true;
        } else if (t == "-h" || t == "--help") {
            o.show_help = true;
        } else if (is_dangerous_flag(t)) {
            o.dangerous_mode = true;
        } else if (t == "--thinking") {
            o.include_thinking = true;
        } else if (t == "-c" || t == "--continue") {
            o.continue_latest = true;
        } else if (t == "--resume") {
            if (i + 1 >= tokens.size()) {
                return fail("--resume requires a session id "
                            "(use -c/--continue for the most recent session)");
            }
            o.resume_session_id = tokens[++i];
        } else if (split_eq(t, "--resume", eq_value)) {
            o.resume_session_id = eq_value;
        } else if (t == "--session-id") {
            if (i + 1 >= tokens.size()) return fail("--session-id requires a value");
            o.session_id = tokens[++i];
        } else if (split_eq(t, "--session-id", eq_value)) {
            o.session_id = eq_value;
        } else if (t == "--output-format") {
            if (i + 1 >= tokens.size()) {
                return fail("--output-format requires a value (text|json|stream-json)");
            }
            o.output_format = tokens[++i];
        } else if (split_eq(t, "--output-format", eq_value)) {
            o.output_format = eq_value;
        } else if (t == "--permission-mode") {
            if (i + 1 >= tokens.size()) return fail("--permission-mode requires a value (default|accept-edits|plan|yolo)");
            o.permission_mode = tokens[++i];
        } else if (split_eq(t, "--permission-mode", eq_value)) {
            o.permission_mode = eq_value;
        } else if (t == "--model") {
            if (i + 1 >= tokens.size()) return fail("--model requires a saved model name");
            o.model_name = tokens[++i];
        } else if (split_eq(t, "--model", eq_value)) {
            o.model_name = eq_value;
        } else if (t == "--max-turns") {
            if (i + 1 >= tokens.size()) return fail("--max-turns requires a positive integer");
            if (!parse_max_turns(tokens[++i])) {
                return fail("--max-turns must be a positive integer, got: " + tokens[i]);
            }
        } else if (split_eq(t, "--max-turns", eq_value)) {
            if (!parse_max_turns(eq_value)) {
                return fail("--max-turns must be a positive integer, got: " + eq_value);
            }
        } else if (!t.empty() && t[0] == '-') {
            // 未知 flag:显式报错。脚本场景里静默吞掉拼错的参数(--modle)
            // 会变成难排查的行为差异。
            return fail("unknown option in print mode: " + t);
        } else {
            if (!o.prompt.empty()) {
                return fail("unexpected extra argument: " + t +
                            " (quote the whole prompt as a single argument)");
            }
            o.prompt = t;
        }
    }

    if (!o.permission_mode.empty() && !valid_permission_mode(o.permission_mode)) {
        return fail("invalid --permission-mode: " + o.permission_mode +
                    " (expected default|accept-edits|plan|yolo)");
    }
    // 会话 id 是文件名(<id>.jsonl):字符集校验挡住路径穿越("../x")与
    // 忘带 id 时把 prompt 误当 id 的情况(prompt 几乎必含空格/标点)。
    if (!o.resume_session_id.empty() && !is_valid_session_id_token(o.resume_session_id)) {
        return fail("invalid --resume session id: '" + o.resume_session_id +
                    "' (allowed: letters, digits, '-', '_'; max 64 chars)");
    }
    if (!o.session_id.empty() && !is_valid_session_id_token(o.session_id)) {
        return fail("invalid --session-id: '" + o.session_id +
                    "' (allowed: letters, digits, '-', '_'; max 64 chars)");
    }
    if (!o.session_id.empty() && looks_like_legacy_pid_session_id(o.session_id)) {
        return fail("invalid --session-id: '" + o.session_id +
                    "' collides with the legacy PID-suffixed session filename "
                    "shape; pick a different id");
    }
    if (o.continue_latest && !o.resume_session_id.empty()) {
        return fail("--continue and --resume are mutually exclusive");
    }
    if (!o.session_id.empty() &&
        (o.continue_latest || !o.resume_session_id.empty())) {
        return fail("--session-id names a NEW session and cannot be combined "
                    "with --resume/--continue");
    }
    if (!o.output_format.empty() && o.output_format != "text" &&
        o.output_format != "json" && o.output_format != "stream-json") {
        return fail("invalid --output-format: " + o.output_format +
                    " (expected text|json|stream-json)");
    }
    // --yolo 与 --permission-mode 同时给且矛盾时,尊重更宽的显式意志:
    // dangerous 本来就是"跳过一切确认",permission_mode 保留原值用于
    // plan 等只读约束(dangerous 时 plan 的只读门在 AgentLoop 内已被
    // is_dangerous() 短路,与 TUI -yolo 行为一致)。
    return o;
}

std::string print_mode_usage_line() {
    return "usage: acecode -p [-c | --resume <id> | --session-id <id>] "
           "[--output-format text|json|stream-json] [--thinking] "
           "[--yolo] [--permission-mode <m>] "
           "[--model <name>] [--max-turns <n>] \"<prompt>\"\n"
           "run `acecode -p --help` for details\n";
}

std::string print_mode_help() {
    return
        "acecode -p / --print - headless (non-interactive) print mode\n"
        "\n"
        "Runs one agent turn, prints the final assistant reply to stdout, exits.\n"
        "Every run persists a normal session that can be continued later.\n"
        "\n"
        "Usage:\n"
        "  acecode -p [options] \"<prompt>\"\n"
        "  echo \"prompt\" | acecode -p [options]\n"
        "  git diff | acecode -p \"review this diff\"    (stdin + argument are joined)\n"
        "\n"
        "Options:\n"
        "  -c, --continue           Continue the most recent session in this directory\n"
        "  --resume <id>            Continue the session with the given id\n"
        "  --session-id <id>        Create the new session under a caller-chosen id\n"
        "                           (letters, digits, '-', '_'; max 64 chars)\n"
        "  --output-format <fmt>    text (default) | json | stream-json\n"
        "                           json: stdout gets one result object, e.g.\n"
        "                           {\"type\":\"result\",\"session_id\":\"...\",\"is_error\":false,\"result\":\"...\"}\n"
        "                           stream-json: stdout gets completed-part JSONL\n"
        "                           (step/text/tool/error records, flushed per line)\n"
        "  --thinking              Include completed reasoning records in stream-json\n"
        "  --model <name>           Use a saved model by name (also applies on resume)\n"
        "  --permission-mode <m>    default | accept-edits | plan | yolo\n"
        "                           (also applies on resume; overrides the saved mode)\n"
        "  --max-turns <n>          Cap agent-loop iterations for this turn\n"
        "  --yolo, --dangerous      Skip all permission confirmations\n"
        "  -h, --help               Show this help\n"
        "\n"
        "Multi-turn conversations:\n"
        "  sid=$(acecode -p --output-format json \"step 1\" | jq -r .session_id)\n"
        "  acecode -p --resume \"$sid\" \"step 2\"\n"
        "or with a caller-chosen id (no output parsing needed):\n"
        "  acecode -p --session-id my-task \"step 1\"\n"
        "  acecode -p --resume my-task \"step 2\"\n"
        "or simply continue the latest session of the current directory:\n"
        "  acecode -p -c \"next step\"\n"
        "\n"
        "Exit codes: 0 success, 1 turn failed, 64 usage error, 130 interrupted\n";
}

} // namespace acecode::headless
