// 覆盖 TUI 输入历史导航与 queued 消息召回的纯状态机。
// 这些测试不触碰 FTXUI,只验证 TuiState 中 input_history / pending_queue
// / input_text 的交互契约。

#include <gtest/gtest.h>

#include "tui/input_history_navigation.hpp"

#include <string>
#include <vector>

using acecode::InputMode;
using acecode::TuiState;
using acecode::prepend_mode_prefix;
using acecode::tui::clear_current_input_for_history_restore;
using acecode::tui::navigate_input_history_down;
using acecode::tui::navigate_input_history_up;
using acecode::tui::try_cancel_latest_pending_for_history_text;

namespace {

std::vector<std::string> pending(const TuiState& state) {
    return state.pending_queue;
}

} // namespace

// 场景:pending [A, B] 与 history 尾部 [A, B] 匹配时,ArrowUp 先召回最新 B
// 并只取消队列尾部 B。
TEST(InputHistoryNavigation, ArrowUpCancelsLatestMatchingQueuedMessage) {
    TuiState state;
    state.pending_queue = {"A", "B"};
    state.input_history = {"A", "B"};
    state.input_text = "draft";
    state.input_cursor = state.input_text.size();

    ASSERT_TRUE(navigate_input_history_up(state));

    EXPECT_EQ(state.input_text, "B");
    EXPECT_EQ(state.input_cursor, 1u);
    EXPECT_EQ(state.saved_input, "draft");
    EXPECT_EQ(pending(state), std::vector<std::string>({"A"}));
}

// 场景:多条 queued 消息连续按 ArrowUp,按最新到最早依次取消并召回。
TEST(InputHistoryNavigation, RepeatedArrowUpCancelsQueuedMessagesNewestFirst) {
    TuiState state;
    state.pending_queue = {"A", "B"};
    state.input_history = {"A", "B"};
    state.input_text = "draft";

    ASSERT_TRUE(navigate_input_history_up(state));
    ASSERT_TRUE(navigate_input_history_up(state));

    EXPECT_EQ(state.input_text, "A");
    EXPECT_TRUE(state.pending_queue.empty());
    EXPECT_EQ(state.history_index, 0);
}

// 场景:ArrowDown 保持普通 history 浏览,可从较早召回项回到较新项,再回到草稿。
TEST(InputHistoryNavigation, ArrowDownBrowsesRecalledHistoryBackToSavedDraft) {
    TuiState state;
    state.pending_queue = {"A", "B"};
    state.input_history = {"A", "B"};
    state.input_text = "draft";

    ASSERT_TRUE(navigate_input_history_up(state));
    ASSERT_TRUE(navigate_input_history_up(state));
    ASSERT_TRUE(state.pending_queue.empty());

    ASSERT_TRUE(navigate_input_history_down(state));
    EXPECT_EQ(state.input_text, "B");
    EXPECT_TRUE(state.pending_queue.empty());
    EXPECT_EQ(state.history_index, 1);

    ASSERT_TRUE(navigate_input_history_down(state));
    EXPECT_EQ(state.input_text, "draft");
    EXPECT_TRUE(state.pending_queue.empty());
    EXPECT_EQ(state.history_index, -1);
}

// 场景:history 目标文本与 pending_queue.back() 不完全相同时,不能取消 queue。
TEST(InputHistoryNavigation, ArrowUpDoesNotCancelWhenHistoryTextDoesNotMatchQueueTail) {
    TuiState state;
    state.pending_queue = {"queued"};
    state.input_history = {"other"};

    ASSERT_TRUE(navigate_input_history_up(state));

    EXPECT_EQ(state.input_text, "other");
    EXPECT_EQ(pending(state), std::vector<std::string>({"queued"}));
}

// 场景:队列取消后 ArrowDown 不会把已召回文本自动重新入队。
TEST(InputHistoryNavigation, ArrowDownDoesNotRequeueCancelledMessage) {
    TuiState state;
    state.pending_queue = {"A"};
    state.input_history = {"A"};
    state.input_text = "draft";

    ASSERT_TRUE(navigate_input_history_up(state));
    ASSERT_TRUE(state.pending_queue.empty());

    ASSERT_TRUE(navigate_input_history_down(state));

    EXPECT_EQ(state.input_text, "draft");
    EXPECT_TRUE(state.pending_queue.empty());
}

// 场景:Shell 历史项即使解析后的文本匹配 queue 尾部,也不能取消普通 queued 消息。
TEST(InputHistoryNavigation, ShellHistoryEntryDoesNotCancelQueuedMessage) {
    TuiState state;
    state.pending_queue = {"npm test"};
    state.input_history = {prepend_mode_prefix("npm test", InputMode::Shell)};

    ASSERT_TRUE(navigate_input_history_up(state));

    EXPECT_EQ(state.input_mode, InputMode::Shell);
    EXPECT_EQ(state.input_text, "npm test");
    EXPECT_EQ(pending(state), std::vector<std::string>({"npm test"}));
}

TEST(InputHistoryNavigation, ClearCurrentInputAllowsArrowUpRestore) {
    TuiState state;
    state.input_history = {"older"};
    state.input_text = "draft";
    state.input_cursor = 2;

    ASSERT_TRUE(clear_current_input_for_history_restore(state));

    EXPECT_TRUE(state.input_text.empty());
    EXPECT_EQ(state.input_cursor, 0u);
    EXPECT_EQ(state.input_mode, InputMode::Normal);

    ASSERT_TRUE(navigate_input_history_up(state));

    EXPECT_EQ(state.input_text, "draft");
    EXPECT_EQ(state.input_cursor, 5u);
    EXPECT_EQ(state.input_mode, InputMode::Normal);
    EXPECT_EQ(state.history_index, -1);

    ASSERT_TRUE(navigate_input_history_up(state));
    EXPECT_EQ(state.input_text, "older");
}

TEST(InputHistoryNavigation, ArrowDownAfterClearedInputDoesNothing) {
    TuiState state;
    state.input_history = {"older"};
    state.input_text = "draft";

    ASSERT_TRUE(clear_current_input_for_history_restore(state));

    EXPECT_FALSE(navigate_input_history_down(state));
    EXPECT_TRUE(state.input_text.empty());
    EXPECT_EQ(state.input_mode, InputMode::Normal);

    ASSERT_TRUE(navigate_input_history_up(state));
    EXPECT_EQ(state.input_text, "draft");
}

TEST(InputHistoryNavigation, ClearCurrentShellInputRestoresShellMode) {
    TuiState state;
    state.input_mode = InputMode::Shell;
    state.input_text = "npm test";
    state.input_cursor = state.input_text.size();

    ASSERT_TRUE(clear_current_input_for_history_restore(state));

    EXPECT_TRUE(state.input_text.empty());
    EXPECT_EQ(state.input_mode, InputMode::Normal);

    ASSERT_TRUE(navigate_input_history_up(state));

    EXPECT_EQ(state.input_mode, InputMode::Shell);
    EXPECT_EQ(state.input_text, "npm test");
    EXPECT_EQ(state.input_cursor, 8u);
}

TEST(InputHistoryNavigation, ClearEmptyNormalInputIsNoop) {
    TuiState state;

    EXPECT_FALSE(clear_current_input_for_history_restore(state));
    EXPECT_EQ(state.history_index, -1);
    EXPECT_TRUE(state.saved_input.empty());
}

// 场景:底层取消 helper 只接受精确尾部匹配。
TEST(InputHistoryNavigation, CancelHelperRequiresExactTailMatch) {
    TuiState state;
    state.pending_queue = {"A", "B"};

    EXPECT_FALSE(try_cancel_latest_pending_for_history_text(state, "A"));
    EXPECT_EQ(pending(state), std::vector<std::string>({"A", "B"}));

    EXPECT_TRUE(try_cancel_latest_pending_for_history_text(state, "B"));
    EXPECT_EQ(pending(state), std::vector<std::string>({"A"}));
}
