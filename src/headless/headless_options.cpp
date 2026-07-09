#include "headless_options.hpp"

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

} // namespace

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
        } else if (is_dangerous_flag(t)) {
            o.dangerous_mode = true;
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
    // --yolo 与 --permission-mode 同时给且矛盾时,尊重更宽的显式意志:
    // dangerous 本来就是"跳过一切确认",permission_mode 保留原值用于
    // plan 等只读约束(dangerous 时 plan 的只读门在 AgentLoop 内已被
    // is_dangerous() 短路,与 TUI -yolo 行为一致)。
    return o;
}

} // namespace acecode::headless
