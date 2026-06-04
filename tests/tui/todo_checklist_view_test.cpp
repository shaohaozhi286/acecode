#include <gtest/gtest.h>

#include "tui/todo_checklist_view.hpp"

#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/screen.hpp>

#include <string>
#include <vector>

using acecode::TodoItem;

TEST(TodoChecklistView, PresentsRequestedMarkersAndStyles) {
    std::vector<TodoItem> todos = {
        {"1", "查看工作目录内容", "completed"},
        {"2", "创建测试文件 hello.txt", "in_progress"},
        {"3", "读取并校验文件内容", "pending"},
    };

    auto rows = acecode::tui::todo_checklist_rows(todos, 80);
    ASSERT_EQ(rows.size(), 3u);

    EXPECT_EQ(rows[0].marker,
              std::string(acecode::tui::todo_filled_square_marker()));
    EXPECT_TRUE(rows[0].marker_active);
    EXPECT_TRUE(rows[0].row_bright);
    EXPECT_FALSE(rows[0].strike);

    EXPECT_EQ(rows[1].marker,
              std::string(acecode::tui::todo_open_square_marker()));
    EXPECT_FALSE(rows[1].marker_active);
    EXPECT_FALSE(rows[1].row_bright);
    EXPECT_FALSE(rows[1].strike);

    EXPECT_EQ(rows[2].marker, std::string(acecode::tui::todo_check_marker()));
    EXPECT_TRUE(rows[2].strike);
    EXPECT_FALSE(rows[2].row_bright);
    EXPECT_FALSE(rows[2].marker_active);
}

TEST(TodoChecklistView, UsesSidebarOnlyWhenRegularSidebarVisible) {
    EXPECT_TRUE(acecode::tui::todo_checklist_uses_sidebar(true));
    EXPECT_FALSE(acecode::tui::todo_checklist_uses_sidebar(false));
}

TEST(TodoChecklistView, CompletedStrikeAppliesOnlyToMutedText) {
    std::vector<TodoItem> todos = {
        {"1", "done task", "completed"},
    };

    ftxui::Screen screen(24, 1);
    ftxui::Render(screen, acecode::tui::render_todo_checklist_block(todos, 24));

    int marker_x = -1;
    int text_x = -1;
    for (int x = 0; x < screen.dimx(); ++x) {
        const auto& cell = screen.CellAt(x, 0);
        if (cell.character == acecode::tui::todo_check_marker()) {
            marker_x = x;
        }
        if (cell.character == "d" && text_x < 0) {
            text_x = x;
        }
    }

    ASSERT_GE(marker_x, 0);
    ASSERT_GE(text_x, 0);
    EXPECT_FALSE(screen.CellAt(marker_x, 0).strikethrough);
    EXPECT_TRUE(screen.CellAt(text_x, 0).strikethrough);
    EXPECT_EQ(screen.CellAt(text_x, 0).foreground_color, ftxui::Color::GrayLight);
}

TEST(TodoChecklistView, WrapsLongItemsToAtMostTwoTailTruncatedLines) {
    std::vector<TodoItem> todos = {
        {"1", "1234567890 abcdefghij klmnopqrst", "in_progress"},
    };

    auto rows = acecode::tui::todo_checklist_rows(todos, 10);

    ASSERT_EQ(rows.size(), 1u);
    ASSERT_EQ(rows[0].content_lines.size(), 2u);
    EXPECT_EQ(rows[0].content_lines[0], "1234567890");
    EXPECT_EQ(rows[0].content_lines[1], "abcdefghi\xE2\x80\xA6");
}

TEST(TodoChecklistView, PrioritizesVisibleItemsAndCapsAtTen) {
    std::vector<TodoItem> todos = {
        {"c1", "completed one", "completed"},
        {"p1", "pending one", "pending"},
        {"i1", "progress one", "in_progress"},
        {"x1", "cancelled one", "cancelled"},
        {"p2", "pending two", "pending"},
        {"c2", "completed two", "completed"},
        {"i2", "progress two", "in_progress"},
        {"p3", "pending three", "pending"},
        {"c3", "completed three", "completed"},
        {"p4", "pending four", "pending"},
        {"c4", "completed four", "completed"},
        {"p5", "pending five", "pending"},
    };

    auto rows = acecode::tui::todo_checklist_rows(todos, 80);

    ASSERT_EQ(rows.size(), 10u);
    ASSERT_FALSE(rows[0].content_lines.empty());
    ASSERT_FALSE(rows[1].content_lines.empty());
    EXPECT_EQ(rows[0].content_lines[0], "progress one");
    EXPECT_EQ(rows[1].content_lines[0], "progress two");
    EXPECT_EQ(rows[2].content_lines[0], "pending one");
    EXPECT_EQ(rows[6].content_lines[0], "pending five");
    EXPECT_EQ(rows[7].content_lines[0], "completed one");
    EXPECT_EQ(rows[9].content_lines[0], "completed three");
}

TEST(TodoChecklistView, RenderUsesCompactMarkerPrefixWithoutLeadingPadding) {
    std::vector<TodoItem> todos = {
        {"1", "pending task", "pending"},
    };

    ftxui::Screen screen(24, 1);
    ftxui::Render(screen, acecode::tui::render_todo_checklist_block(todos, 24));

    int marker_x = -1;
    int text_x = -1;
    for (int x = 0; x < screen.dimx(); ++x) {
        const auto& cell = screen.CellAt(x, 0);
        if (cell.character == acecode::tui::todo_open_square_marker()) {
            marker_x = x;
        }
        if (cell.character == "p" && text_x < 0) {
            text_x = x;
        }
    }

    EXPECT_EQ(marker_x, 0);
    EXPECT_EQ(text_x, 2);
}
