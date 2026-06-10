#include "session_title_generator.hpp"

#include "../utils/encoding.hpp"
#include "../utils/terminal_title.hpp"

#include <algorithm>
#include <cctype>
#include <nlohmann/json.hpp>
#include <vector>

namespace acecode {
namespace {

constexpr std::size_t kMaxGeneratedTitleBytes = 120;

std::string trim_ascii(std::string s) {
    auto is_space = [](unsigned char c) { return std::isspace(c) != 0; };
    while (!s.empty() && is_space(static_cast<unsigned char>(s.front()))) s.erase(s.begin());
    while (!s.empty() && is_space(static_cast<unsigned char>(s.back()))) s.pop_back();
    return s;
}

std::string collapse_whitespace(std::string s) {
    std::string out;
    out.reserve(s.size());
    bool in_space = false;
    for (unsigned char c : s) {
        if (std::isspace(c)) {
            if (!out.empty() && !in_space) out.push_back(' ');
            in_space = true;
        } else {
            out.push_back(static_cast<char>(c));
            in_space = false;
        }
    }
    if (!out.empty() && out.back() == ' ') out.pop_back();
    return out;
}

std::string strip_wrapping_quotes(std::string s) {
    s = trim_ascii(std::move(s));
    while (s.size() >= 2 &&
           ((s.front() == '"' && s.back() == '"') ||
            (s.front() == '\'' && s.back() == '\''))) {
        s = trim_ascii(s.substr(1, s.size() - 2));
    }
    return s;
}

std::string title_from_json_or_raw(const std::string& raw) {
    try {
        auto j = nlohmann::json::parse(raw);
        if (j.is_object()) {
            for (const auto* key : {"title", "name", "summary"}) {
                if (j.contains(key) && j[key].is_string()) {
                    return j[key].get<std::string>();
                }
            }
        }
        if (j.is_string()) return j.get<std::string>();
    } catch (...) {
    }
    return raw;
}

std::string strip_common_prefix(std::string s) {
    const std::string lower = [&] {
        std::string v = s;
        std::transform(v.begin(), v.end(), v.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        return v;
    }();
    for (const auto* prefix : {"title:", "session title:", "标题:"}) {
        const std::string p(prefix);
        if (lower.rfind(p, 0) == 0) {
            return trim_ascii(s.substr(p.size()));
        }
    }
    return s;
}

} // namespace

std::string sanitize_generated_session_title(std::string raw) {
    std::string title = title_from_json_or_raw(ensure_utf8(raw));
    title = strip_wrapping_quotes(strip_common_prefix(collapse_whitespace(title)));
    if (title.empty()) return {};
    title = truncate_utf8_prefix(title, kMaxGeneratedTitleBytes, "");
    std::string err;
    if (!sanitize_title(title, err)) return {};
    return trim_ascii(title);
}

std::optional<std::string> generate_session_title(
    LlmProvider& provider,
    const std::string& first_user_text,
    int max_input_bytes) {
    const int bounded_input = std::max(1, max_input_bytes);
    const std::string input = truncate_utf8_prefix(
        first_user_text,
        static_cast<std::size_t>(bounded_input),
        "");
    if (trim_ascii(input).empty()) return std::nullopt;

    ChatMessage system;
    system.role = "system";
    system.content =
        "Generate a concise title for this coding-agent session. "
        "Return only JSON like {\"title\":\"...\"}. "
        "Use at most 8 English words or 24 Chinese characters. "
        "Do not include punctuation unless needed for a file or symbol name.";

    ChatMessage user;
    user.role = "user";
    user.content = input;

    ChatResponse response = provider.chat({system, user}, {});
    std::string title = sanitize_generated_session_title(response.content);
    if (title.empty()) return std::nullopt;
    return title;
}

} // namespace acecode
