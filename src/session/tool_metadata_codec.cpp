// 见 tool_metadata_codec.hpp 头注释。本文件实现两类纯函数 encode/decode。
// 关键是 decode 路径绝不抛异常,坏数据一律降级为 std::nullopt。

#include "tool_metadata_codec.hpp"

#include <string>

namespace acecode {

namespace {

const char* kind_to_string(DiffLineKind k) {
    switch (k) {
        case DiffLineKind::Context: return "context";
        case DiffLineKind::Added:   return "added";
        case DiffLineKind::Removed: return "removed";
    }
    return "context";
}

// "context"/"added"/"removed" 三个串之外的输入直接让 decode 整体降级。
std::optional<DiffLineKind> parse_kind(const std::string& s) {
    if (s == "context") return DiffLineKind::Context;
    if (s == "added")   return DiffLineKind::Added;
    if (s == "removed") return DiffLineKind::Removed;
    return std::nullopt;
}

} // namespace

nlohmann::json encode_tool_summary(const ToolSummary& s) {
    nlohmann::json obj;
    obj["verb"]   = s.verb;
    obj["object"] = s.object;
    obj["icon"]   = s.icon;

    nlohmann::json metrics_arr = nlohmann::json::array();
    for (const auto& kv : s.metrics) {
        nlohmann::json pair = nlohmann::json::array();
        pair.push_back(kv.first);
        pair.push_back(kv.second);
        metrics_arr.push_back(std::move(pair));
    }
    obj["metrics"] = std::move(metrics_arr);
    return obj;
}

std::optional<ToolSummary> decode_tool_summary(const nlohmann::json& j) {
    if (!j.is_object()) return std::nullopt;

    ToolSummary s;

    // 字段缺失允许(取默认空字符串);字段存在时类型必须正确,否则降级。
    if (j.contains("verb")) {
        if (!j["verb"].is_string()) return std::nullopt;
        s.verb = j["verb"].get<std::string>();
    }
    if (j.contains("object")) {
        if (!j["object"].is_string()) return std::nullopt;
        s.object = j["object"].get<std::string>();
    }
    if (j.contains("icon")) {
        if (!j["icon"].is_string()) return std::nullopt;
        s.icon = j["icon"].get<std::string>();
    }
    if (j.contains("metrics")) {
        const auto& m = j["metrics"];
        if (!m.is_array()) return std::nullopt;
        for (const auto& item : m) {
            if (!item.is_array() || item.size() != 2) return std::nullopt;
            if (!item[0].is_string() || !item[1].is_string()) return std::nullopt;
            s.metrics.emplace_back(item[0].get<std::string>(),
                                   item[1].get<std::string>());
        }
    }

    return s;
}

nlohmann::json encode_tool_hunks(const std::vector<DiffHunk>& h) {
    nlohmann::json arr = nlohmann::json::array();
    for (const auto& hunk : h) {
        nlohmann::json obj;
        obj["old_start"] = hunk.old_start;
        obj["old_count"] = hunk.old_count;
        obj["new_start"] = hunk.new_start;
        obj["new_count"] = hunk.new_count;

        nlohmann::json lines_arr = nlohmann::json::array();
        for (const auto& line : hunk.lines) {
            nlohmann::json line_obj;
            line_obj["kind"] = kind_to_string(line.kind);
            line_obj["text"] = line.text;
            // optional<int>:有值才输出键,无值不输出(避免 null 歧义)。
            if (line.old_line_no.has_value()) {
                line_obj["old_line_no"] = *line.old_line_no;
            }
            if (line.new_line_no.has_value()) {
                line_obj["new_line_no"] = *line.new_line_no;
            }
            lines_arr.push_back(std::move(line_obj));
        }
        obj["lines"] = std::move(lines_arr);
        arr.push_back(std::move(obj));
    }
    return arr;
}

std::optional<std::vector<DiffHunk>> decode_tool_hunks(const nlohmann::json& j) {
    if (!j.is_array()) return std::nullopt;

    std::vector<DiffHunk> hunks;
    hunks.reserve(j.size());

    for (const auto& hj : j) {
        if (!hj.is_object()) return std::nullopt;

        // 必需字段:四个整数 + lines 数组。任一缺失/类型错 → 全局降级。
        if (!hj.contains("old_start") || !hj["old_start"].is_number_integer() ||
            !hj.contains("old_count") || !hj["old_count"].is_number_integer() ||
            !hj.contains("new_start") || !hj["new_start"].is_number_integer() ||
            !hj.contains("new_count") || !hj["new_count"].is_number_integer() ||
            !hj.contains("lines")     || !hj["lines"].is_array()) {
            return std::nullopt;
        }

        DiffHunk hunk;
        hunk.old_start = hj["old_start"].get<int>();
        hunk.old_count = hj["old_count"].get<int>();
        hunk.new_start = hj["new_start"].get<int>();
        hunk.new_count = hj["new_count"].get<int>();

        for (const auto& lj : hj["lines"]) {
            if (!lj.is_object()) return std::nullopt;
            if (!lj.contains("kind") || !lj["kind"].is_string()) return std::nullopt;
            if (!lj.contains("text") || !lj["text"].is_string()) return std::nullopt;

            auto k = parse_kind(lj["kind"].get<std::string>());
            if (!k.has_value()) return std::nullopt;

            DiffLine line;
            line.kind = *k;
            line.text = lj["text"].get<std::string>();
            if (lj.contains("old_line_no")) {
                if (!lj["old_line_no"].is_number_integer()) return std::nullopt;
                line.old_line_no = lj["old_line_no"].get<int>();
            }
            if (lj.contains("new_line_no")) {
                if (!lj["new_line_no"].is_number_integer()) return std::nullopt;
                line.new_line_no = lj["new_line_no"].get<int>();
            }
            // 未知额外字段(future-version 扩展)被默默忽略,向前兼容。

            hunk.lines.push_back(std::move(line));
        }

        hunks.push_back(std::move(hunk));
    }

    return hunks;
}

} // namespace acecode
