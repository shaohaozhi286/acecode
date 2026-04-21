// 覆盖 src/tool/word_diff.cpp 的纯函数 — token 切分、LCS 对齐、
// 变化占比计算、超长行早退这四大路径。
// 渲染层(src/tui/diff_view.cpp)决定何时启用词级高亮,这里只保证
// 上游函数在各输入下输出的段序列符合预期。
//
// 场景一览:
//   1. 全同:单段 Same,覆盖全字符串
//   2. 纯尾部追加:前缀 Same + 尾部 Added
//   3. 中间小改(一个 token 被替换):Same-Removed-Added-Same 序列
//   4. 变化占比判定:助手函数对小改 / 大改的阈值行为
//   5. 超长 token 数触发早退:返回 {Removed, Added} 两段

#include <gtest/gtest.h>

#include "tool/word_diff.hpp"

#include <string>

using acecode::tokenize_for_word_diff;
using acecode::word_diff;
using acecode::word_diff_below_threshold;
using acecode::word_diff_change_ratio;
using acecode::WordDiffKind;
using acecode::WordDiffSegment;

// 场景 1:两侧完全相同,期望单段 Same 覆盖整个文本。
TEST(WordDiff, AllSameSingleSegment) {
    auto segs = word_diff("foo(a, b)", "foo(a, b)");
    ASSERT_EQ(segs.size(), 1u);
    EXPECT_EQ(segs[0].kind, WordDiffKind::Same);
    EXPECT_EQ(segs[0].value, "foo(a, b)");
    EXPECT_DOUBLE_EQ(word_diff_change_ratio(segs), 0.0);
}

// 场景 2:新串在末尾追加内容。期望前缀合并为 Same,后缀为 Added。
TEST(WordDiff, PureTailAddition) {
    auto segs = word_diff("hello", "hello world");
    // 可能拆成 Same("hello") + Added(" world") 或更细,最终合并后
    // 应只剩两段:Same + Added。
    ASSERT_EQ(segs.size(), 2u);
    EXPECT_EQ(segs[0].kind, WordDiffKind::Same);
    EXPECT_EQ(segs[0].value, "hello");
    EXPECT_EQ(segs[1].kind, WordDiffKind::Added);
    EXPECT_EQ(segs[1].value, " world");
}

// 场景 3:仅最后一个实参不同 —— foo(a, b) → foo(a, c)。
// 预期 Same("foo(a, ") + Removed("b") + Added("c") + Same(")")。
TEST(WordDiff, MiddleSmallChangeTriggersWordLevel) {
    auto segs = word_diff("foo(a, b)", "foo(a, c)");
    // 至少应该存在一个 Removed "b" 和一个 Added "c";
    // 字符串总长相同,变化比例应远小于 40%。
    bool saw_removed_b = false;
    bool saw_added_c = false;
    for (const auto& s : segs) {
        if (s.kind == WordDiffKind::Removed && s.value == "b") saw_removed_b = true;
        if (s.kind == WordDiffKind::Added && s.value == "c") saw_added_c = true;
    }
    EXPECT_TRUE(saw_removed_b);
    EXPECT_TRUE(saw_added_c);

    double ratio = word_diff_change_ratio(segs);
    EXPECT_LT(ratio, 0.4);
    EXPECT_TRUE(word_diff_below_threshold(ratio));
}

// 场景 4:完全不同的两行,变化占比应 > 40%,阈值助手返回 false。
TEST(WordDiff, BigChangeExceedsThreshold) {
    auto segs = word_diff("int x = 1;", "std::cout << foo(bar);");
    double ratio = word_diff_change_ratio(segs);
    EXPECT_GT(ratio, 0.4);
    EXPECT_FALSE(word_diff_below_threshold(ratio));
}

// 场景 5:超长 token 数触发早退 —— 返回 {Removed a}{Added b} 两段,
// 不再做 LCS 分解。
TEST(WordDiff, LongInputEarlyReturn) {
    // 构造一条有 600+ 个 token 的长行(交替 word/space),超过默认上限 200。
    std::string a;
    for (int i = 0; i < 600; ++i) {
        a += "x ";
    }
    std::string b = a + "y"; // 仅尾部加一个字符

    auto segs = word_diff(a, b);
    ASSERT_EQ(segs.size(), 2u);
    EXPECT_EQ(segs[0].kind, WordDiffKind::Removed);
    EXPECT_EQ(segs[0].value, a);
    EXPECT_EQ(segs[1].kind, WordDiffKind::Added);
    EXPECT_EQ(segs[1].value, b);
}

// 场景 5b:显式把上限设得很低(如 2),同样会触发早退。
// 这是为了固化 `max_tokens_per_side` 参数的行为。
TEST(WordDiff, CustomMaxTokensEarlyReturn) {
    auto segs = word_diff("a b c d", "a b c e", /*max_tokens_per_side=*/2);
    ASSERT_EQ(segs.size(), 2u);
    EXPECT_EQ(segs[0].kind, WordDiffKind::Removed);
    EXPECT_EQ(segs[1].kind, WordDiffKind::Added);
}

// tokenize 的基础性质:token 个数 > 0,所有 token 连起来等于原文本。
TEST(WordDiff, TokenizeRoundtripsInput) {
    const std::string src = "foo(a, b) + 3";
    auto tokens = tokenize_for_word_diff(src);
    EXPECT_FALSE(tokens.empty());
    std::string joined;
    for (const auto& t : tokens) joined += t;
    EXPECT_EQ(joined, src);
}
