#include "agent_loop_doom_guard.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <sstream>

namespace acecode {

namespace {

std::string ascii_lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

std::string collapse_space(const std::string& value) {
    std::string out;
    bool previous_space = false;
    for (unsigned char c : value) {
        if (std::isspace(c)) {
            if (!previous_space && !out.empty()) out.push_back(' ');
            previous_space = true;
            continue;
        }
        out.push_back(static_cast<char>(c));
        previous_space = false;
    }
    if (!out.empty() && out.back() == ' ') out.pop_back();
    return out;
}

std::string normalize_command(std::string value) {
    std::replace(value.begin(), value.end(), '\\', '/');
    value = ascii_lower(std::move(value));
    return collapse_space(value);
}

std::string strip_wrapping_quotes(std::string value) {
    while (value.size() >= 2 &&
           ((value.front() == '"' && value.back() == '"') ||
            (value.front() == '\'' && value.back() == '\''))) {
        value = value.substr(1, value.size() - 2);
    }
    return value;
}

bool looks_like_path(const std::string& token) {
    if (token.size() < 3) return false;
    if (token.size() >= 3 &&
        std::isalpha(static_cast<unsigned char>(token[0])) &&
        token[1] == ':' &&
        (token[2] == '/' || token[2] == '\\')) {
        return true;
    }
    if (token.rfind("./", 0) == 0 || token.rfind("../", 0) == 0) return true;
    if (token.find('/') == std::string::npos && token.find('\\') == std::string::npos) {
        return false;
    }
    static const char* exts[] = {
        ".md", ".txt", ".json", ".jsonl", ".cpp", ".hpp", ".h", ".c",
        ".py", ".js", ".ts", ".tsx", ".jsx", ".cs", ".java", ".xml",
        ".yaml", ".yml", ".ini", ".log", ".csv", ".html", ".css"
    };
    const std::string lower = ascii_lower(token);
    for (const char* ext : exts) {
        if (lower.find(ext) != std::string::npos) return true;
    }
    return lower.find('/') != std::string::npos || lower.find('\\') != std::string::npos;
}

std::vector<std::string> command_tokens_preserving_quotes(const std::string& command) {
    std::vector<std::string> tokens;
    std::string current;
    char quote = '\0';
    for (char c : command) {
        if (quote != '\0') {
            if (c == quote) {
                quote = '\0';
                continue;
            }
            current.push_back(c);
            continue;
        }
        if (c == '"' || c == '\'') {
            quote = c;
            continue;
        }
        if (std::isspace(static_cast<unsigned char>(c)) || c == ',' || c == ';') {
            if (!current.empty()) {
                tokens.push_back(current);
                current.clear();
            }
            continue;
        }
        current.push_back(c);
    }
    if (!current.empty()) tokens.push_back(current);
    return tokens;
}

std::string extract_target_from_command(const std::string& command) {
    for (std::size_t i = 0; i + 2 < command.size(); ++i) {
        if (!std::isalpha(static_cast<unsigned char>(command[i])) ||
            command[i + 1] != ':' ||
            (command[i + 2] != '\\' && command[i + 2] != '/')) {
            continue;
        }
        std::size_t end = i + 3;
        while (end < command.size()) {
            char c = command[end];
            if (std::isspace(static_cast<unsigned char>(c)) ||
                c == '"' || c == '\'' || c == '|' || c == ')' ||
                c == ']' || c == '}' || c == ',' || c == ';') {
                break;
            }
            ++end;
        }
        std::string path = command.substr(i, end - i);
        std::replace(path.begin(), path.end(), '\\', '/');
        return ascii_lower(std::move(path));
    }

    for (std::string token : command_tokens_preserving_quotes(command)) {
        token = strip_wrapping_quotes(std::move(token));
        while (!token.empty() &&
               (token.back() == ')' || token.back() == ']' || token.back() == '}')) {
            token.pop_back();
        }
        while (!token.empty() &&
               (token.front() == '(' || token.front() == '[' || token.front() == '{')) {
            token.erase(token.begin());
        }
        if (!looks_like_path(token)) continue;
        std::replace(token.begin(), token.end(), '\\', '/');
        return ascii_lower(std::move(token));
    }
    return {};
}

bool contains_any(const std::string& value, std::initializer_list<const char*> needles) {
    for (const char* needle : needles) {
        if (value.find(needle) != std::string::npos) return true;
    }
    return false;
}

AgentLoopDoomGuard::Operation classify_bash_operation(const std::string& normalized_command) {
    if (contains_any(normalized_command, {
            "set-content", "out-file", "add-content", "writealltext",
            "write_text", "open(", "replace(", "move ", "copy ", ">>", ">"
        })) {
        return AgentLoopDoomGuard::Operation::Write;
    }
    if (contains_any(normalized_command, {
            "findstr", "select-string", "grep", "rg ", "search", "contains",
            "re.search", "indexof", "match"
        })) {
        return AgentLoopDoomGuard::Operation::Search;
    }
    if (contains_any(normalized_command, {
            "old remaining", "remaining:", "verify", "check", "compare",
            "diff", "fc "
        })) {
        return AgentLoopDoomGuard::Operation::Verify;
    }
    if (contains_any(normalized_command, {
            "type ", "get-content", "cat ", "readalltext", ".read()",
            "read_text", "head ", "tail "
        })) {
        return AgentLoopDoomGuard::Operation::Read;
    }
    return AgentLoopDoomGuard::Operation::Unknown;
}

// 自带明确恢复路径的前置条件失败:错误信息本身就指示"先补一次 file_read /
// 换正确的 hash,然后原样重试"。模型按提示重试时参数往往与上一次完全相同,
// 若把这类失败计入 attempts,exact-repeat 拦截会把唯一正确的恢复路径堵死
// (典型:old_string 编辑因 "only partially read" 失败 → 补全量读取 → 同参重试被 Guarded)。
bool is_retryable_precondition_failure(const ToolResult& result) {
    if (result.success) return false;
    const std::string lower = ascii_lower(result.output);
    return contains_any(lower, {
        // ToolErrors::file_not_read_for_edit / file_partially_read_for_edit
        "read the full file with file_read",
        // ToolErrors::external_modification
        "re-read the file before writing",
        // file_edit range 模式 hash 过期:错误里附带了当前 hash 与内容,重试是预期路径,
        // 且拦截后给出的合成结果反而丢失这些恢复信息
        "range hash mismatch"
    });
}

std::string operation_name(AgentLoopDoomGuard::Operation op) {
    switch (op) {
    case AgentLoopDoomGuard::Operation::Read: return "read";
    case AgentLoopDoomGuard::Operation::Search: return "search";
    case AgentLoopDoomGuard::Operation::Verify: return "verify";
    case AgentLoopDoomGuard::Operation::Write: return "write";
    case AgentLoopDoomGuard::Operation::Unknown: return "unknown";
    }
    return "unknown";
}

} // namespace

void AgentLoopDoomGuard::begin_model_turn() {
    for (auto it = cooldown_turns_.begin(); it != cooldown_turns_.end();) {
        if (it->second <= 0) {
            it = cooldown_turns_.erase(it);
            continue;
        }
        --it->second;
        if (it->second <= 0) it = cooldown_turns_.erase(it);
        else ++it;
    }
}

std::optional<ToolResult> AgentLoopDoomGuard::maybe_guard(const ToolCall& call) {
    CallKey key = build_key(call);
    if (cooldown_active(key.tool)) {
        return make_synthetic_result(
            key,
            "the tool is temporarily cooled down after repeated low-signal attempts",
            true);
    }

    if (low_signal_exact_count(key) >= 1) {
        return make_synthetic_result(
            key,
            "the exact same tool call already produced a low-signal result",
            false);
    }

    if (key.tool == "bash" &&
        key.operation != Operation::Write &&
        !key.semantic.empty() &&
        low_signal_semantic_count(key) >= 2) {
        start_cooldown("bash", 2);
        return make_synthetic_result(
            key,
            "similar shell attempts against the same target already produced low-signal results",
            true);
    }

    return std::nullopt;
}

void AgentLoopDoomGuard::record_result(const ToolCall& call, const ToolResult& result) {
    CallKey key = build_key(call);
    ResultClass result_class = classify_result(result);
    if (result_class == ResultClass::Useful) {
        attempts_.erase(
            std::remove_if(attempts_.begin(), attempts_.end(), [&](const Attempt& attempt) {
                return attempt.key.exact == key.exact ||
                       (!key.semantic.empty() && attempt.key.semantic == key.semantic);
            }),
            attempts_.end());
    }
    if (is_retryable_precondition_failure(result)) {
        return;
    }
    attempts_.push_back(Attempt{std::move(key), result_class, is_low_signal(result_class)});
    constexpr std::size_t kMaxAttempts = 64;
    if (attempts_.size() > kMaxAttempts) {
        attempts_.erase(attempts_.begin(), attempts_.begin() + (attempts_.size() - kMaxAttempts));
    }
}

AgentLoopDoomGuard::CallKey AgentLoopDoomGuard::build_key(const ToolCall& call) const {
    CallKey key;
    key.tool = ascii_lower(call.function_name);
    key.exact = key.tool + "\n" + collapse_space(call.function_arguments);

    nlohmann::json args = nlohmann::json::parse(call.function_arguments, nullptr, false);
    if (args.is_object()) {
        if (args.contains("file_path") && args["file_path"].is_string()) {
            key.target = normalize_command(args["file_path"].get<std::string>());
        } else if (args.contains("path") && args["path"].is_string()) {
            key.target = normalize_command(args["path"].get<std::string>());
        }
    }

    if (key.tool == "bash" && args.is_object() &&
        args.contains("command") && args["command"].is_string()) {
        const std::string command = args["command"].get<std::string>();
        const std::string normalized_command = normalize_command(command);
        key.exact = key.tool + "\n" + normalized_command;
        if (key.target.empty()) key.target = extract_target_from_command(command);
        key.operation = classify_bash_operation(normalized_command);
        if (!key.target.empty() && key.operation != Operation::Unknown) {
            key.semantic = key.tool + "\n" + operation_name(key.operation) + "\n" + key.target;
        }
        return key;
    }

    if (!key.target.empty()) {
        key.semantic = key.tool + "\n" + key.target;
    }
    return key;
}

AgentLoopDoomGuard::ResultClass AgentLoopDoomGuard::classify_result(const ToolResult& result) const {
    std::string output = collapse_space(result.output);
    std::string lower = ascii_lower(output);
    if (lower.find("[doom-loop guard]") != std::string::npos) return ResultClass::Guarded;
    // 成功结果不做关键词细分:file_read/grep 等工具的输出就是文件内容本身,正常内容里
    // 出现 "timed out"/"超时"/"not found" 等词(典型如 i18n 错误文案文件)不代表调用失败。
    // 关键词只能用来给"已经失败"的结果分细类,否则一次成功读取会被记成 low-signal,
    // 后续合法的重复读取被整体拦截,模型失去对文件状态的感知。
    if (result.success) {
        if (output.empty() || lower == "(no output)" || lower == "no output") {
            return ResultClass::Empty;
        }
        return ResultClass::Useful;
    }
    if (contains_any(lower, {
            "access denied", "permission denied",
            "\xE6\x8B\x92\xE7\xBB\x9D\xE8\xAE\xBF\xE9\x97\xAE"
        })) {
        return ResultClass::Denied;
    }
    if (contains_any(lower, {
            "no such file", "not found", "cannot find",
            "\xE6\x89\xBE\xE4\xB8\x8D\xE5\x88\xB0\xE6\x8C\x87\xE5\xAE\x9A"
            "\xE7\x9A\x84\xE6\x96\x87\xE4\xBB\xB6",
            "\xE7\xB3\xBB\xE7\xBB\x9F\xE6\x89\xBE\xE4\xB8\x8D\xE5\x88\xB0",
            "\xE4\xB8\x8D\xE5\xAD\x98\xE5\x9C\xA8"
        })) {
        return ResultClass::NotFound;
    }
    if (contains_any(lower, {"timed out", "timeout", "\xE8\xB6\x85\xE6\x97\xB6"})) {
        return ResultClass::Timeout;
    }
    if (contains_any(lower, {
            "old remaining: true", "remaining: true", "unchanged",
            "no changes", "0 replacements", "found 0 matches"
        })) {
        return ResultClass::Unchanged;
    }
    // 走到这里一定是失败结果且未命中任何细分关键词。
    return ResultClass::Error;
}

bool AgentLoopDoomGuard::is_low_signal(ResultClass result) const {
    return result != ResultClass::Useful;
}

ToolResult AgentLoopDoomGuard::make_synthetic_result(const CallKey& key,
                                                     const std::string& reason,
                                                     bool cooldown_active_flag) const {
    std::ostringstream oss;
    oss << "[Doom-loop guard] Skipped repeated tool call: " << reason << ".";
    if (!key.target.empty()) {
        oss << "\nTarget: " << key.target;
    }
    if (key.operation != Operation::Unknown) {
        oss << "\nIntent: " << operation_name(key.operation);
    }
    if (cooldown_active_flag && key.tool == "bash") {
        oss << "\nThe bash tool is temporarily cooled down for this task. "
            << "Use existing evidence or a dedicated non-shell tool such as file_read or grep when applicable.";
    } else {
        oss << "\nUse the previous result, choose a different approach, or answer with the evidence already available.";
    }
    ToolResult result;
    result.output = oss.str();
    result.success = false;
    ToolSummary summary;
    summary.verb = "Guarded";
    summary.object = key.tool.empty() ? "tool call" : key.tool;
    summary.metrics.push_back({"reason", cooldown_active_flag ? "cooldown" : "repeat"});
    summary.icon = "!";
    result.summary = std::move(summary);
    return result;
}

int AgentLoopDoomGuard::low_signal_exact_count(const CallKey& key) const {
    int count = 0;
    for (const Attempt& attempt : attempts_) {
        if (attempt.low_signal &&
            attempt.key.tool == key.tool &&
            attempt.key.exact == key.exact) {
            ++count;
        }
    }
    return count;
}

int AgentLoopDoomGuard::low_signal_semantic_count(const CallKey& key) const {
    if (key.semantic.empty()) return 0;
    int count = 0;
    for (const Attempt& attempt : attempts_) {
        if (attempt.low_signal &&
            attempt.key.semantic == key.semantic &&
            attempt.key.operation != Operation::Write) {
            ++count;
        }
    }
    return count;
}

void AgentLoopDoomGuard::start_cooldown(const std::string& tool, int turns) {
    if (turns <= 0) return;
    auto& current = cooldown_turns_[tool];
    current = std::max(current, turns);
}

bool AgentLoopDoomGuard::cooldown_active(const std::string& tool) const {
    auto it = cooldown_turns_.find(tool);
    return it != cooldown_turns_.end() && it->second > 0;
}

} // namespace acecode
