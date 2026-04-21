#include "memory_index.hpp"

#include <sstream>
#include <unordered_map>
#include <unordered_set>

namespace acecode {

namespace {

// Extract the entry name from a managed index line. A managed line starts
// with "- [" and contains "](<stem>.md)". Returns empty string when the line
// is not a managed entry reference so free-form user notes survive untouched.
std::string extract_name_from_line(const std::string& line) {
    // Require a leading "- [" preceded only by whitespace to avoid mangling
    // lines like "  note: we have -[old] references".
    std::size_t i = 0;
    while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) ++i;
    if (i >= line.size() || line[i] != '-') return {};
    ++i;
    if (i >= line.size() || line[i] != ' ') return {};
    ++i;
    if (i >= line.size() || line[i] != '[') return {};

    // Find the matching ](<...>.md).
    std::size_t close_bracket = line.find(']', i);
    if (close_bracket == std::string::npos) return {};
    if (close_bracket + 1 >= line.size() || line[close_bracket + 1] != '(') return {};
    std::size_t close_paren = line.find(')', close_bracket + 2);
    if (close_paren == std::string::npos) return {};

    std::string target = line.substr(close_bracket + 2,
                                     close_paren - (close_bracket + 2));
    const std::string ext = ".md";
    if (target.size() <= ext.size()) return {};
    if (target.compare(target.size() - ext.size(), ext.size(), ext) != 0) return {};
    return target.substr(0, target.size() - ext.size());
}

std::vector<std::string> split_lines_keep_endings(const std::string& text) {
    std::vector<std::string> out;
    std::size_t start = 0;
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '\n') {
            out.push_back(text.substr(start, i - start + 1));
            start = i + 1;
        }
    }
    if (start < text.size()) {
        out.push_back(text.substr(start));
    }
    return out;
}

} // namespace

std::string format_memory_index_line(const MemoryEntry& entry) {
    // em-dash separator mirrors the Claude Code convention in MEMORY.md hook lines.
    return "- [" + entry.description + "](" + entry.name + ".md) \xE2\x80\x94 "
           + entry.description;
}

std::string render_memory_index(const std::vector<MemoryEntry>& entries,
                                const std::string& existing_text) {
    std::unordered_map<std::string, const MemoryEntry*> by_name;
    by_name.reserve(entries.size());
    for (const auto& e : entries) by_name[e.name] = &e;

    std::unordered_set<std::string> rewritten;

    std::vector<std::string> lines;
    if (!existing_text.empty()) {
        lines = split_lines_keep_endings(existing_text);
    }

    std::ostringstream oss;
    bool produced_any = false;

    if (lines.empty()) {
        for (const auto& entry : entries) {
            oss << format_memory_index_line(entry) << '\n';
            rewritten.insert(entry.name);
            produced_any = true;
        }
    } else {
        for (const auto& raw_line : lines) {
            std::string body = raw_line;
            std::string ending;
            if (!body.empty() && body.back() == '\n') {
                ending = "\n";
                body.pop_back();
            }

            std::string name = extract_name_from_line(body);
            if (name.empty()) {
                oss << body << (ending.empty() ? std::string{} : ending);
                produced_any = true;
                continue;
            }
            auto it = by_name.find(name);
            if (it == by_name.end()) {
                // Stale entry: drop the line entirely.
                continue;
            }
            oss << format_memory_index_line(*it->second) << (ending.empty() ? "\n" : ending);
            rewritten.insert(name);
            produced_any = true;
        }

        for (const auto& entry : entries) {
            if (rewritten.count(entry.name)) continue;
            if (produced_any) {
                oss << format_memory_index_line(entry) << '\n';
            } else {
                oss << format_memory_index_line(entry) << '\n';
                produced_any = true;
            }
        }
    }

    std::string out = oss.str();
    if (!out.empty() && out.back() != '\n') out.push_back('\n');
    return out;
}

std::string remove_memory_index_line(const std::string& existing_text,
                                     const std::string& name) {
    if (existing_text.empty()) return existing_text;
    std::vector<std::string> lines = split_lines_keep_endings(existing_text);
    std::ostringstream oss;
    for (const auto& raw : lines) {
        std::string body = raw;
        std::string ending;
        if (!body.empty() && body.back() == '\n') {
            ending = "\n";
            body.pop_back();
        }
        if (extract_name_from_line(body) == name) continue;
        oss << body << ending;
    }
    return oss.str();
}

} // namespace acecode
