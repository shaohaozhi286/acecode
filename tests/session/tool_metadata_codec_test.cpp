// 覆盖 ToolSummary / vector<DiffHunk> 与 nlohmann::json 的双向编解码 round-trip
// 与边界场景。这是 resume 完整还原(restore-tool-calls-on-resume)的基石:
// 一旦 codec round-trip 失真,resume 后的 diff/摘要会跟原会话对不上。
//
// 失败优先级:任何 round-trip 测试红 → codec bug,必须先修;任何 decode 降级
// 测试红 → 安全降级路径破坏,可能让坏数据导致整次 resume 崩溃。

#include <gtest/gtest.h>

#include "session/tool_metadata_codec.hpp"
#include "tool/tool_executor.hpp"
#include "tool/diff_utils.hpp"

#include <nlohmann/json.hpp>

using acecode::DiffHunk;
using acecode::DiffLine;
using acecode::DiffLineKind;
using acecode::ToolSummary;
using acecode::decode_tool_hunks;
using acecode::decode_tool_summary;
using acecode::encode_tool_hunks;
using acecode::encode_tool_summary;

namespace {

// 辅助:构造一条 context 行(text + 双侧行号)。
DiffLine make_context(const std::string& text, int old_no, int new_no) {
    DiffLine l;
    l.kind = DiffLineKind::Context;
    l.text = text;
    l.old_line_no = old_no;
    l.new_line_no = new_no;
    return l;
}

DiffLine make_added(const std::string& text, int new_no) {
    DiffLine l;
    l.kind = DiffLineKind::Added;
    l.text = text;
    l.new_line_no = new_no;
    return l;
}

DiffLine make_removed(const std::string& text, int old_no) {
    DiffLine l;
    l.kind = DiffLineKind::Removed;
    l.text = text;
    l.old_line_no = old_no;
    return l;
}

void expect_summary_equal(const ToolSummary& a, const ToolSummary& b) {
    EXPECT_EQ(a.verb, b.verb);
    EXPECT_EQ(a.object, b.object);
    EXPECT_EQ(a.icon, b.icon);
    ASSERT_EQ(a.metrics.size(), b.metrics.size());
    for (size_t i = 0; i < a.metrics.size(); ++i) {
        EXPECT_EQ(a.metrics[i].first,  b.metrics[i].first);
        EXPECT_EQ(a.metrics[i].second, b.metrics[i].second);
    }
}

void expect_hunks_equal(const std::vector<DiffHunk>& a, const std::vector<DiffHunk>& b) {
    ASSERT_EQ(a.size(), b.size());
    for (size_t hi = 0; hi < a.size(); ++hi) {
        EXPECT_EQ(a[hi].old_start, b[hi].old_start);
        EXPECT_EQ(a[hi].old_count, b[hi].old_count);
        EXPECT_EQ(a[hi].new_start, b[hi].new_start);
        EXPECT_EQ(a[hi].new_count, b[hi].new_count);
        ASSERT_EQ(a[hi].lines.size(), b[hi].lines.size());
        for (size_t li = 0; li < a[hi].lines.size(); ++li) {
            EXPECT_EQ(static_cast<int>(a[hi].lines[li].kind),
                      static_cast<int>(b[hi].lines[li].kind));
            EXPECT_EQ(a[hi].lines[li].text, b[hi].lines[li].text);
            EXPECT_EQ(a[hi].lines[li].old_line_no.has_value(),
                      b[hi].lines[li].old_line_no.has_value());
            if (a[hi].lines[li].old_line_no.has_value()) {
                EXPECT_EQ(*a[hi].lines[li].old_line_no, *b[hi].lines[li].old_line_no);
            }
            EXPECT_EQ(a[hi].lines[li].new_line_no.has_value(),
                      b[hi].lines[li].new_line_no.has_value());
            if (a[hi].lines[li].new_line_no.has_value()) {
                EXPECT_EQ(*a[hi].lines[li].new_line_no, *b[hi].lines[li].new_line_no);
            }
        }
    }
}

} // namespace

// ============================================================================
// ToolSummary round-trip
// ============================================================================

// 验证最常见 file_edit 摘要双向 round-trip 无损。
TEST(ToolMetadataCodecSummary, RoundTripTypical) {
    ToolSummary s;
    s.verb   = "Edited";
    s.object = "src/foo.cpp";
    s.metrics = {{"+", "12"}, {"-", "3"}};
    s.icon   = "✎";

    auto j = encode_tool_summary(s);
    auto out = decode_tool_summary(j);

    ASSERT_TRUE(out.has_value());
    expect_summary_equal(s, *out);
}

// 无 metrics 的轻量摘要也应 round-trip。
TEST(ToolMetadataCodecSummary, RoundTripEmptyMetrics) {
    ToolSummary s;
    s.verb   = "Read";
    s.object = "src/main.cpp";
    s.icon   = "→";
    // metrics 留空

    auto j = encode_tool_summary(s);
    EXPECT_TRUE(j.contains("metrics"));
    EXPECT_TRUE(j["metrics"].is_array());
    EXPECT_EQ(j["metrics"].size(), 0u);

    auto out = decode_tool_summary(j);
    ASSERT_TRUE(out.has_value());
    expect_summary_equal(s, *out);
}

// 类型错应该安全降级,不影响整次 resume。
TEST(ToolMetadataCodecSummary, DecodeInvalidReturnsNullopt) {
    nlohmann::json j;
    j["verb"]   = 123;            // 数字不是字符串
    j["object"] = "src/foo.cpp";
    j["icon"]   = "✎";

    auto out = decode_tool_summary(j);
    EXPECT_FALSE(out.has_value());
}

// 多种坏形态:整体不是对象、metrics 不是数组、metrics 元素不是 [string,string]。
TEST(ToolMetadataCodecSummary, DecodeMisshapedReturnsNullopt) {
    EXPECT_FALSE(decode_tool_summary(nlohmann::json::array()).has_value());

    nlohmann::json j1;
    j1["verb"] = "Edited";
    j1["metrics"] = "not-an-array";
    EXPECT_FALSE(decode_tool_summary(j1).has_value());

    nlohmann::json j2;
    j2["verb"] = "Edited";
    j2["metrics"] = nlohmann::json::array();
    j2["metrics"].push_back("solo-string"); // 不是 2-tuple
    EXPECT_FALSE(decode_tool_summary(j2).has_value());

    nlohmann::json j3;
    j3["verb"] = "Edited";
    j3["metrics"] = nlohmann::json::array();
    j3["metrics"].push_back(nlohmann::json::array({"+", 12})); // 第二项不是 string
    EXPECT_FALSE(decode_tool_summary(j3).has_value());
}

// ============================================================================
// vector<DiffHunk> round-trip
// ============================================================================

// 纯 context 行 round-trip,行号必须保留。
TEST(ToolMetadataCodecHunks, RoundTripPureContext) {
    DiffHunk h;
    h.old_start = 1; h.old_count = 3;
    h.new_start = 1; h.new_count = 3;
    h.lines.push_back(make_context("aaa", 1, 1));
    h.lines.push_back(make_context("bbb", 2, 2));
    h.lines.push_back(make_context("ccc", 3, 3));

    std::vector<DiffHunk> in{h};
    auto j = encode_tool_hunks(in);
    auto out = decode_tool_hunks(j);
    ASSERT_TRUE(out.has_value());
    expect_hunks_equal(in, *out);
}

// 全 added 行(old_line_no 为空),验证 optional 字段在缺席时不被错误填充。
TEST(ToolMetadataCodecHunks, RoundTripPureAddition) {
    DiffHunk h;
    h.old_start = 0; h.old_count = 0;
    h.new_start = 1; h.new_count = 2;
    h.lines.push_back(make_added("new line 1", 1));
    h.lines.push_back(make_added("new line 2", 2));

    std::vector<DiffHunk> in{h};
    auto j = encode_tool_hunks(in);

    // encode 不应输出 old_line_no 字段。
    for (const auto& l : j[0]["lines"]) {
        EXPECT_FALSE(l.contains("old_line_no"));
        EXPECT_TRUE(l.contains("new_line_no"));
    }

    auto out = decode_tool_hunks(j);
    ASSERT_TRUE(out.has_value());
    expect_hunks_equal(in, *out);
    for (const auto& l : (*out)[0].lines) {
        EXPECT_FALSE(l.old_line_no.has_value());
        EXPECT_TRUE(l.new_line_no.has_value());
    }
}

// 全 removed 行(new_line_no 为空)。
TEST(ToolMetadataCodecHunks, RoundTripPureDeletion) {
    DiffHunk h;
    h.old_start = 5; h.old_count = 2;
    h.new_start = 5; h.new_count = 0;
    h.lines.push_back(make_removed("gone 1", 5));
    h.lines.push_back(make_removed("gone 2", 6));

    std::vector<DiffHunk> in{h};
    auto j = encode_tool_hunks(in);

    for (const auto& l : j[0]["lines"]) {
        EXPECT_TRUE(l.contains("old_line_no"));
        EXPECT_FALSE(l.contains("new_line_no"));
    }

    auto out = decode_tool_hunks(j);
    ASSERT_TRUE(out.has_value());
    expect_hunks_equal(in, *out);
}

// 模拟真实 file_edit diff 的 round-trip(三种 kind 混合,15 行)。
TEST(ToolMetadataCodecHunks, RoundTripMixed) {
    DiffHunk h;
    h.old_start = 10; h.old_count = 5;
    h.new_start = 10; h.new_count = 7;
    h.lines.push_back(make_context("ctx1", 10, 10));
    h.lines.push_back(make_context("ctx2", 11, 11));
    h.lines.push_back(make_removed("old1", 12));
    h.lines.push_back(make_removed("old2", 13));
    h.lines.push_back(make_added("new1", 12));
    h.lines.push_back(make_added("new2", 13));
    h.lines.push_back(make_added("new3", 14));
    h.lines.push_back(make_added("new4", 15));
    h.lines.push_back(make_context("ctx3", 14, 16));
    // 留点空隙,让 lines.size() 不等于 old_count + new_count
    h.lines.push_back(make_context("ctx4", 15, 17));

    std::vector<DiffHunk> in{h};
    auto j = encode_tool_hunks(in);
    auto out = decode_tool_hunks(j);
    ASSERT_TRUE(out.has_value());
    expect_hunks_equal(in, *out);
}

// 一个 file 多个 hunk(模拟非连续修改)。
TEST(ToolMetadataCodecHunks, RoundTripMultiHunk) {
    DiffHunk h1;
    h1.old_start = 1; h1.old_count = 1;
    h1.new_start = 1; h1.new_count = 2;
    h1.lines.push_back(make_removed("first", 1));
    h1.lines.push_back(make_added("first-new1", 1));
    h1.lines.push_back(make_added("first-new2", 2));

    DiffHunk h2;
    h2.old_start = 50; h2.old_count = 2;
    h2.new_start = 51; h2.new_count = 1;
    h2.lines.push_back(make_removed("a", 50));
    h2.lines.push_back(make_removed("b", 51));
    h2.lines.push_back(make_added("ab", 51));

    std::vector<DiffHunk> in{h1, h2};
    auto j = encode_tool_hunks(in);
    auto out = decode_tool_hunks(j);
    ASSERT_TRUE(out.has_value());
    expect_hunks_equal(in, *out);
}

// 空 vector,encode 输出 [],decode 出空 vector。
TEST(ToolMetadataCodecHunks, RoundTripEmptyVector) {
    std::vector<DiffHunk> in;
    auto j = encode_tool_hunks(in);
    EXPECT_TRUE(j.is_array());
    EXPECT_EQ(j.size(), 0u);

    auto out = decode_tool_hunks(j);
    ASSERT_TRUE(out.has_value());
    EXPECT_TRUE(out->empty());
}

// 单行 kind="weird",验证安全降级。
TEST(ToolMetadataCodecHunks, DecodeUnknownKindReturnsNullopt) {
    nlohmann::json j;
    j = nlohmann::json::array();
    j.push_back({
        {"old_start", 1}, {"old_count", 1},
        {"new_start", 1}, {"new_count", 1},
        {"lines", nlohmann::json::array({
            {{"kind", "weird"}, {"text", "?"}}
        })}
    });

    auto out = decode_tool_hunks(j);
    EXPECT_FALSE(out.has_value());
}

// hunk 缺 old_start,降级。
TEST(ToolMetadataCodecHunks, DecodeMissingRequiredFieldReturnsNullopt) {
    nlohmann::json j = nlohmann::json::array();
    j.push_back({
        // {"old_start", 1},  // 故意缺
        {"old_count", 1},
        {"new_start", 1},
        {"new_count", 1},
        {"lines", nlohmann::json::array()}
    });

    auto out = decode_tool_hunks(j);
    EXPECT_FALSE(out.has_value());
}

// hunk 多了一个 future-version 字段,decode 应该忽略不报错(向前兼容性 hatch)。
TEST(ToolMetadataCodecHunks, DecodeUnknownExtraFieldIgnored) {
    DiffHunk h;
    h.old_start = 1; h.old_count = 1;
    h.new_start = 1; h.new_count = 1;
    h.lines.push_back(make_context("only", 1, 1));

    std::vector<DiffHunk> in{h};
    auto j = encode_tool_hunks(in);
    j[0]["future_field"] = "future_value";              // 未来扩展字段
    j[0]["lines"][0]["future_line_field"] = 42;         // 行级未来扩展

    auto out = decode_tool_hunks(j);
    ASSERT_TRUE(out.has_value());
    expect_hunks_equal(in, *out);
}

// 顶层不是 array 直接降级。
TEST(ToolMetadataCodecHunks, DecodeNonArrayReturnsNullopt) {
    EXPECT_FALSE(decode_tool_hunks(nlohmann::json::object()).has_value());
    EXPECT_FALSE(decode_tool_hunks(nlohmann::json("string")).has_value());
    EXPECT_FALSE(decode_tool_hunks(nlohmann::json(42)).has_value());
}
