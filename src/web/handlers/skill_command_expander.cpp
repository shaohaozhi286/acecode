#include "skill_command_expander.hpp"

#include "../../skills/skill_activation.hpp"
#include "../../skills/skill_registry.hpp"

#include <cctype>

namespace acecode::web {

namespace {

bool is_ws(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

// 从 original_text 解析首段:从首字符 `/` 之后到首个空白(空格/tab/换行)或末尾。
// 返回 (head_token, args_after_head_with_leading_ws_trimmed)。
//   "/foo bar baz"  → ("foo", "bar baz")
//   "/foo"          → ("foo", "")
//   "/foo   bar"    → ("foo", "bar")(args 起头空白被吃掉)
//   "/"             → ("", "")
//   "hello"         → ("", "")(由调用方先判 first char == '/')
std::pair<std::string, std::string> split_first_token(const std::string& s) {
    if (s.empty() || s[0] != '/') return {"", ""};
    size_t i = 1;
    while (i < s.size() && !is_ws(s[i])) ++i;
    std::string head = s.substr(1, i - 1);
    while (i < s.size() && is_ws(s[i])) ++i;
    std::string args = (i < s.size()) ? s.substr(i) : std::string{};
    return {std::move(head), std::move(args)};
}

} // namespace

SkillExpansionResult try_expand_skill_command(const std::string& original_text,
                                                const SkillRegistry& registry) {
    if (original_text.empty() || original_text[0] != '/') {
        return {false, original_text, ""};
    }
    auto [head, args] = split_first_token(original_text);
    if (head.empty()) {
        return {false, original_text, ""};
    }

    auto meta_opt = registry.find(head);
    if (!meta_opt) {
        // 不在 registry 里 — 可能是 builtin (init/compact) 或纯粹未知名,均透传。
        return {false, original_text, head};
    }
    const auto& meta = *meta_opt;
    // 轻量提示;LLM 按需用 skill_view tool 加载 SKILL.md。详见 skill_activation.hpp 注释。
    std::string expanded = build_skill_invocation_hint(meta, args);

    return {true, std::move(expanded), meta.name};
}

} // namespace acecode::web
