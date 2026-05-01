// 覆盖 src/tui/paste_handler.cpp 的纯字符串与状态机逻辑。
// 这里不触碰 FTXUI 渲染或事件循环，只验证 normalize、line counting、
// 占位符 format/parse、known/unknown 占位符 expand、PlaceholderSpan 查找、
// PasteAccumulator 状态机的 begin/end/in-paste Return/Tab 行为。

#include <gtest/gtest.h>

#include "tui/paste_handler.hpp"

#include <map>
#include <string>

using acecode::tui::count_newlines;
using acecode::tui::expand_placeholders;
using acecode::tui::find_all_placeholders;
using acecode::tui::find_known_placeholders;
using acecode::tui::format_placeholder;
using acecode::tui::kBracketedPasteBegin;
using acecode::tui::kBracketedPasteEnd;
using acecode::tui::normalize_pasted_text;
using acecode::tui::PasteAccumulator;
using acecode::tui::PlaceholderSpan;
using acecode::tui::placeholder_ending_at;
using acecode::tui::placeholder_starting_at;
using acecode::tui::prune_unreferenced;
using acecode::tui::should_fold_to_placeholder;

// 场景：CRLF 与裸 CR 都应统一成 \n（spec scenario "CRLF paste is normalized"）。
TEST(PasteHandlerNormalize, CrlfAndCrAreNormalizedToLf) {
    EXPECT_EQ(normalize_pasted_text("a\r\nb\rc"), "a\nb\nc");
    EXPECT_EQ(normalize_pasted_text("\r\n\r\n"), "\n\n");
}

// 场景：tab 必须扩成 4 个空格（spec scenario "Tabs are expanded"）。
TEST(PasteHandlerNormalize, TabsExpandToFourSpaces) {
    EXPECT_EQ(normalize_pasted_text("a\tb"), "a    b");
    EXPECT_EQ(normalize_pasted_text("\t"), "    ");
}

// 场景：CSI / OSC 等 ANSI 控制序列要被剥掉，只留下可见文本（spec scenario
// "ANSI control sequences are removed"）。
// 注意：C++ \x 转义是贪婪的（\x07b → 单字节 0x7B）；用字符串字面量拼接强制断开。
TEST(PasteHandlerNormalize, AnsiCsiAndOscAreStripped) {
    // CSI: SGR 颜色 + 普通文本
    EXPECT_EQ(normalize_pasted_text("\x1b[31m" "red" "\x1b[0m"), "red");
    // CSI 多参数 + 终止符
    EXPECT_EQ(normalize_pasted_text("\x1b[1;31;4m" "hi" "\x1b[0m"), "hi");
    // OSC by BEL：title 设置
    EXPECT_EQ(normalize_pasted_text("a\x1b]0;title\x07" "b"), "ab");
    // OSC by ST：title 设置
    EXPECT_EQ(normalize_pasted_text("a\x1b]0;t\x1b\\b"), "ab");
}

// 场景：除 \n 外的 C0 控制字符应被丢弃，避免噪音字符进入 input_text。
TEST(PasteHandlerNormalize, OtherC0ControlsAreDropped) {
    // \x01 (^A) / \x02 (^B) 等。同样防止贪婪 \x 转义。
    EXPECT_EQ(normalize_pasted_text("\x01" "a" "\x02" "b"), "ab");
    // \n 必须保留
    EXPECT_EQ(normalize_pasted_text("a\nb"), "a\nb");
}

// 场景：count_newlines 是分隔符个数（spec scenario "Four-line paste"
// 期望 +3 lines for "a\nb\nc\nd"）。
TEST(PasteHandlerLineCount, NewlinesAreSeparatorCount) {
    EXPECT_EQ(count_newlines(""), 0);
    EXPECT_EQ(count_newlines("hello"), 0);
    EXPECT_EQ(count_newlines("a\nb"), 1);
    EXPECT_EQ(count_newlines("a\nb\nc\nd"), 3);
    // CRLF 算一次
    EXPECT_EQ(count_newlines("a\r\nb\r\nc"), 2);
    // 裸 CR 算一次
    EXPECT_EQ(count_newlines("a\rb"), 1);
}

// 场景：阈值边界——恰好 800 字节单行不折叠，>800 字节折叠；<=2 换行不折叠，>2 折叠。
TEST(PasteHandlerThreshold, FoldsAboveByteOrNewlineThreshold) {
    // 800 字节单行（恰等于阈值）不折叠
    EXPECT_FALSE(should_fold_to_placeholder(std::string(800, 'a')));
    // 801 字节单行折叠
    EXPECT_TRUE(should_fold_to_placeholder(std::string(801, 'a')));
    // 2 个换行（3 行）不折叠
    EXPECT_FALSE(should_fold_to_placeholder("a\nb\nc"));
    // 3 个换行（4 行）折叠（spec scenario "Four-line paste"）
    EXPECT_TRUE(should_fold_to_placeholder("a\nb\nc\nd"));
    // 短单行不折叠
    EXPECT_FALSE(should_fold_to_placeholder("hello world"));
}

// 场景：format_placeholder 在 newline_count==0 与 >0 时格式不同。
TEST(PasteHandlerFormat, PlaceholderFormatVariants) {
    EXPECT_EQ(format_placeholder(5, 0), "[Pasted text #5]");
    EXPECT_EQ(format_placeholder(5, 3), "[Pasted text #5 +3 lines]");
    EXPECT_EQ(format_placeholder(99, 47), "[Pasted text #99 +47 lines]");
}

// 场景：find_all_placeholders 列出所有形态，按出现顺序，不区分 store。
TEST(PasteHandlerFind, FindAllPlaceholdersInOrder) {
    const std::string text = "head [Pasted text #1] mid [Pasted text #2 +5 lines] tail";
    auto spans = find_all_placeholders(text);
    ASSERT_EQ(spans.size(), 2u);
    EXPECT_EQ(spans[0].paste_id, 1);
    EXPECT_EQ(text.substr(spans[0].begin, spans[0].end - spans[0].begin),
              "[Pasted text #1]");
    EXPECT_EQ(spans[1].paste_id, 2);
    EXPECT_EQ(text.substr(spans[1].begin, spans[1].end - spans[1].begin),
              "[Pasted text #2 +5 lines]");
}

// 场景：find_known_placeholders 仅返回 store 里有的 id（光标原子跨越的依据）。
TEST(PasteHandlerFind, FindKnownFiltersByStore) {
    const std::string text = "[Pasted text #1] and [Pasted text #99 +3 lines]";
    std::map<int, std::string> store;
    store[1] = "real";
    auto spans = find_known_placeholders(text, store);
    ASSERT_EQ(spans.size(), 1u);
    EXPECT_EQ(spans[0].paste_id, 1);
}

// 场景：placeholder_ending_at / starting_at 给 Backspace / Delete / Arrow 用。
TEST(PasteHandlerFind, PlaceholderSpanLookupAtBoundary) {
    const std::string text = "x [Pasted text #1] y";
    std::map<int, std::string> store{{1, "ignored"}};

    // 占位符位于 [2, 18)，"]" 在 17，end == 18
    EXPECT_FALSE(placeholder_ending_at(text, store, 17).has_value());
    auto end_at = placeholder_ending_at(text, store, 18);
    ASSERT_TRUE(end_at.has_value());
    EXPECT_EQ(end_at->paste_id, 1);
    EXPECT_EQ(end_at->begin, 2u);
    EXPECT_EQ(end_at->end, 18u);

    auto start_at = placeholder_starting_at(text, store, 2);
    ASSERT_TRUE(start_at.has_value());
    EXPECT_EQ(start_at->paste_id, 1);
    EXPECT_EQ(start_at->begin, 2u);
    EXPECT_EQ(start_at->end, 18u);

    // 中间位置不返回
    EXPECT_FALSE(placeholder_ending_at(text, store, 10).has_value());
    EXPECT_FALSE(placeholder_starting_at(text, store, 10).has_value());

    // 未知 id 即便位置匹配也不返回（只有 known span 是原子）
    std::map<int, std::string> empty_store;
    EXPECT_FALSE(placeholder_ending_at(text, empty_store, 18).has_value());
    EXPECT_FALSE(placeholder_starting_at(text, empty_store, 2).has_value());
}

// 场景：submit 时已知 id 展开为原文（spec scenario "Placeholder expands on submit"）。
TEST(PasteHandlerExpand, KnownPlaceholdersExpand) {
    std::map<int, std::string> store;
    store[1] = "a\nb\nc\nd";
    const std::string visible = "please inspect [Pasted text #1 +3 lines]";
    EXPECT_EQ(expand_placeholders(visible, store), "please inspect a\nb\nc\nd");
}

// 场景：未知 id 保持字面（spec scenario "Unknown placeholder remains literal"）。
TEST(PasteHandlerExpand, UnknownPlaceholdersStayLiteral) {
    std::map<int, std::string> store;
    const std::string visible = "keep [Pasted text #99 +3 lines] literal";
    EXPECT_EQ(expand_placeholders(visible, store), visible);
}

// 场景：expand 应尊重 span 边界做 splice，而不是简单字符串替换——这样
// 即便 stored content 自身又包含 "[Pasted text #N]" 字面也不会被二次匹配。
TEST(PasteHandlerExpand, ExpandIsSpliceBased) {
    std::map<int, std::string> store;
    store[1] = "[Pasted text #1] inside"; // 极端情况：原文含同 id 字面
    const std::string visible = "x [Pasted text #1] y";
    // 期望：第一个占位符替换成 store[1] 的字面；不再二次扫描 store[1] 中的同名 token
    EXPECT_EQ(expand_placeholders(visible, store), "x [Pasted text #1] inside y");
}

// 场景：prune_unreferenced 把孤儿条目清掉，input_text 中仍引用的 id 保留。
TEST(PasteHandlerPrune, OrphansAreRemoved) {
    std::map<int, std::string> store;
    store[1] = "first";
    store[2] = "second";
    store[3] = "third";
    prune_unreferenced(store, "still has [Pasted text #2 +1 lines] only");
    EXPECT_EQ(store.size(), 1u);
    EXPECT_TRUE(store.count(2) == 1);
    EXPECT_FALSE(store.count(1));
    EXPECT_FALSE(store.count(3));

    // 输入清空 → 全部清掉
    prune_unreferenced(store, "");
    EXPECT_TRUE(store.empty());
}

// 场景：bracketed paste 状态机基础流程——begin / 字符 / end，期间 in_paste 为 true。
TEST(PasteAccumulatorTest, BeginCharactersEnd) {
    PasteAccumulator acc;
    EXPECT_FALSE(acc.in_paste());

    auto r = acc.feed_special(kBracketedPasteBegin);
    EXPECT_TRUE(r.consume);
    EXPECT_FALSE(r.just_completed);
    EXPECT_TRUE(acc.in_paste());

    r = acc.feed_character("hello");
    EXPECT_TRUE(r.consume);
    EXPECT_FALSE(r.just_completed);
    EXPECT_TRUE(acc.in_paste());

    r = acc.feed_special(kBracketedPasteEnd);
    EXPECT_TRUE(r.consume);
    EXPECT_TRUE(r.just_completed);
    EXPECT_FALSE(acc.in_paste());
    EXPECT_EQ(r.completed_text, "hello");
}

// 场景：paste 内的 Return 不能触发 submit，必须当 \n 进 buffer
// （spec scenario "Pasted newline does not submit"）。
TEST(PasteAccumulatorTest, ReturnInsidePasteAppendsNewline) {
    PasteAccumulator acc;
    acc.feed_special(kBracketedPasteBegin);
    acc.feed_character("line one");
    auto r = acc.feed_special("\n"); // FTXUI Event::Return 的 input() 即 "\n"
    EXPECT_TRUE(r.consume);
    EXPECT_FALSE(r.just_completed);
    acc.feed_character("line two");
    r = acc.feed_special(kBracketedPasteEnd);
    EXPECT_TRUE(r.just_completed);
    EXPECT_EQ(r.completed_text, "line one\nline two");
}

// 场景：paste 内的 Tab 也被吞，最终 normalize 成 4 空格。
TEST(PasteAccumulatorTest, TabInsidePasteIsExpanded) {
    PasteAccumulator acc;
    acc.feed_special(kBracketedPasteBegin);
    acc.feed_character("a");
    acc.feed_special("\t"); // FTXUI Event::Tab 的 input() 即 "\t"
    acc.feed_character("b");
    auto r = acc.feed_special(kBracketedPasteEnd);
    EXPECT_TRUE(r.just_completed);
    EXPECT_EQ(r.completed_text, "a    b");
}

// 场景：空 paste（spec scenario "Empty paste is ignored"）——begin 紧跟 end，
// completed_text 为空，调用方据此跳过 insert。
TEST(PasteAccumulatorTest, EmptyPasteCompletesWithEmptyText) {
    PasteAccumulator acc;
    acc.feed_special(kBracketedPasteBegin);
    auto r = acc.feed_special(kBracketedPasteEnd);
    EXPECT_TRUE(r.consume);
    EXPECT_TRUE(r.just_completed);
    EXPECT_EQ(r.completed_text, "");
    EXPECT_FALSE(acc.in_paste());
}

// 场景：非 paste 状态下普通 special / character 事件不被吞，让外层正常处理。
TEST(PasteAccumulatorTest, NonPasteEventsPassThrough) {
    PasteAccumulator acc;
    EXPECT_FALSE(acc.feed_special("\n").consume);
    EXPECT_FALSE(acc.feed_special("\t").consume);
    EXPECT_FALSE(acc.feed_character("x").consume);
}

// 场景：paste 内嵌的 ANSI CSI bytes 在 normalize 阶段被剥掉。
TEST(PasteAccumulatorTest, EmbeddedAnsiInPasteIsStrippedOnComplete) {
    PasteAccumulator acc;
    acc.feed_special(kBracketedPasteBegin);
    acc.feed_character("hello ");
    acc.feed_special("\x1b[31m"); // 一个完整的 SGR red
    acc.feed_character("world");
    acc.feed_special("\x1b[0m");
    auto r = acc.feed_special(kBracketedPasteEnd);
    EXPECT_TRUE(r.just_completed);
    EXPECT_EQ(r.completed_text, "hello world");
}

// 场景：reset() 清掉中途未结束的 buffer，再次 begin 不污染。
TEST(PasteAccumulatorTest, ResetDropsInFlightBuffer) {
    PasteAccumulator acc;
    acc.feed_special(kBracketedPasteBegin);
    acc.feed_character("garbage");
    acc.reset();
    EXPECT_FALSE(acc.in_paste());

    acc.feed_special(kBracketedPasteBegin);
    acc.feed_character("clean");
    auto r = acc.feed_special(kBracketedPasteEnd);
    EXPECT_TRUE(r.just_completed);
    EXPECT_EQ(r.completed_text, "clean");
}
