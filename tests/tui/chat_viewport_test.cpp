#include <gtest/gtest.h>

#include "tool/ask_user_question_tool.hpp"
#include "tui/ask_question_overlay.hpp"
#include "tui/chat_scroll.hpp"
#include "tui/chat_viewport.hpp"

#include <ftxui/component/event.hpp>
#include <ftxui/component/mouse.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/dom/node.hpp>
#include <ftxui/screen/box.hpp>
#include <ftxui/screen/screen.hpp>

#include <sstream>
#include <string>
#include <vector>

using acecode::AskOption;
using acecode::AskQuestion;
using acecode::tui::ChatViewport;
using acecode::tui::ChatViewportMessageInput;
using ftxui::Event;
using ftxui::HEIGHT;
using ftxui::Mouse;
using ftxui::Render;
using ftxui::Screen;
using ftxui::WIDTH;

namespace {

ChatViewportMessageInput msg(std::string role, std::string content) {
    ChatViewportMessageInput out;
    out.role = std::move(role);
    out.content = std::move(content);
    return out;
}

Event mouse_event(Mouse::Button button,
                  Mouse::Motion motion,
                  int x,
                  int y) {
    Mouse mouse;
    mouse.button = button;
    mouse.motion = motion;
    mouse.x = x;
    mouse.y = y;
    return Event::Mouse("", mouse);
}

std::vector<ChatViewportMessageInput> numbered_messages(int count) {
    std::vector<ChatViewportMessageInput> messages;
    messages.reserve(static_cast<std::size_t>(count));
    for (int i = 0; i < count; ++i) {
        std::ostringstream content;
        content << "message " << i << "\nrow b\nrow c";
        messages.push_back(msg(i % 2 == 0 ? "user" : "assistant",
                               content.str()));
    }
    return messages;
}

AskQuestion ask_question() {
    AskQuestion q;
    q.header = "Decision";
    q.question = "Which implementation path should be used?";
    q.options = {
        AskOption{"Keep current scope", "Use the isolated viewport control."},
        AskOption{"Broader refactor", "Move every overlay into a shared stack."},
    };
    return q;
}

bool valid_box(const ftxui::Box& box) {
    return box.x_min <= box.x_max && box.y_min <= box.y_max;
}

bool boxes_intersect(const ftxui::Box& a, const ftxui::Box& b) {
    return valid_box(a) && valid_box(b) &&
           a.x_min <= b.x_max && b.x_min <= a.x_max &&
           a.y_min <= b.y_max && b.y_min <= a.y_max;
}

} // namespace

TEST(ChatViewport, RendersVisibleRowsInsideFixedBox) {
    ChatViewport viewport;
    viewport.set_messages({
        msg("user", "alpha"),
        msg("assistant", "bravo\ncharlie\ndelta\necho"),
    });

    Screen screen(30, 4);
    Render(screen, viewport.Render());

    const std::string out = screen.ToString();
    EXPECT_NE(out.find("charlie"), std::string::npos);
    EXPECT_NE(out.find("delta"), std::string::npos);
    EXPECT_NE(out.find("echo"), std::string::npos);
    EXPECT_EQ(viewport.chat_box().x_min, 0);
    EXPECT_EQ(viewport.chat_box().x_max, 26);
    EXPECT_EQ(viewport.scrollbar_box().x_min, 27);
    EXPECT_EQ(viewport.scrollbar_box().x_max, 29);
    EXPECT_TRUE(viewport.is_at_tail());
}

TEST(ChatViewport, ShortTranscriptIsBottomAnchored) {
    ChatViewport viewport;
    viewport.set_messages({msg("assistant", "bottom")});

    Screen screen(24, 4);
    Render(screen, viewport.Render());

    EXPECT_NE(screen.CellAt(3, 0).character, "b");
    EXPECT_NE(screen.CellAt(3, 1).character, "b");
    EXPECT_EQ(screen.CellAt(3, 2).character, "b");
}

TEST(ChatViewport, ComposesWithSurroundingFtxuiLayout) {
    ChatViewport viewport;
    viewport.set_messages(numbered_messages(6));

    ftxui::Box header_box;
    ftxui::Box viewport_slot_box;
    ftxui::Box sidebar_box;
    ftxui::Box input_box;
    ftxui::Box overlay_box;

    auto app = ftxui::dbox({
        ftxui::vbox({
            ftxui::text("header") | ftxui::reflect(header_box),
            ftxui::hbox({
                viewport.Render() | ftxui::reflect(viewport_slot_box) |
                    ftxui::flex,
                ftxui::text("sidebar") | ftxui::size(WIDTH, ftxui::EQUAL, 8) |
                    ftxui::reflect(sidebar_box),
            }) | ftxui::flex,
            ftxui::text("input") | ftxui::reflect(input_box),
        }),
        ftxui::text("overlay") | ftxui::reflect(overlay_box),
    });

    Screen screen(60, 12);
    Render(screen, app);

    EXPECT_EQ(header_box.y_min, 0);
    EXPECT_EQ(input_box.y_max, 11);
    EXPECT_EQ(viewport_slot_box.y_min, 1);
    EXPECT_EQ(viewport_slot_box.y_max, 10);
    EXPECT_EQ(sidebar_box.x_min, 52);
    EXPECT_EQ(sidebar_box.x_max, 59);
    EXPECT_EQ(viewport.chat_box().x_min, viewport_slot_box.x_min);
    EXPECT_EQ(viewport.scrollbar_box().x_max, viewport_slot_box.x_max);
    EXPECT_EQ(overlay_box.x_min, 0);
    EXPECT_EQ(overlay_box.y_min, 0);
}

TEST(ChatViewport, AskOverlayKeepsIndependentBoxesWhenComposed) {
    ChatViewport viewport;
    viewport.set_messages(numbered_messages(12));

    ftxui::Box header_box;
    ftxui::Box viewport_slot_box;
    ftxui::Box sidebar_box;
    ftxui::Box input_box;
    ftxui::Box ask_overlay_box;
    ftxui::Box ask_scrollbar_box;

    AskQuestion question = ask_question();
    acecode::tui::AskOverlayLayoutInput layout_input;
    layout_input.question = &question;
    layout_input.current_question_index = 0;
    layout_input.total_questions = 1;
    layout_input.option_focus = 0;
    layout_input.multi_selected.assign(question.options.size(), false);
    layout_input.content_width = 42;
    const auto layout = acecode::tui::build_ask_overlay_layout(layout_input);

    ftxui::Elements ask_rows;
    for (const auto& row : layout.rows) {
        ask_rows.push_back(ftxui::text(row.text));
    }
    ftxui::Elements ask_bar_rows;
    for (std::size_t i = 0; i < ask_rows.size(); ++i) {
        ask_bar_rows.push_back(ftxui::text(i == 0 ? " | " : "   "));
    }

    auto app = ftxui::dbox({
        ftxui::vbox({
            ftxui::text("header") | ftxui::reflect(header_box),
            ftxui::hbox({
                viewport.Render() | ftxui::reflect(viewport_slot_box) |
                    ftxui::flex,
                ftxui::text("sidebar") | ftxui::size(WIDTH, ftxui::EQUAL, 10) |
                    ftxui::reflect(sidebar_box),
            }) | ftxui::flex,
            ftxui::text("input") | ftxui::reflect(input_box),
        }),
        ftxui::hbox({
            ftxui::vbox(std::move(ask_rows)) | ftxui::flex,
            ftxui::vbox(std::move(ask_bar_rows)) |
                ftxui::size(WIDTH, ftxui::EQUAL, 3) |
                ftxui::reflect(ask_scrollbar_box),
        }) | ftxui::border |
            ftxui::size(WIDTH, ftxui::EQUAL, 48) |
            ftxui::size(HEIGHT, ftxui::LESS_THAN, 12) |
            ftxui::reflect(ask_overlay_box),
    });

    Screen screen(80, 18);
    Render(screen, app);

    EXPECT_EQ(header_box.y_min, 0);
    EXPECT_EQ(input_box.y_max, 17);
    EXPECT_EQ(viewport.chat_box().x_min, viewport_slot_box.x_min);
    EXPECT_EQ(viewport.scrollbar_box().x_max, viewport_slot_box.x_max);
    EXPECT_EQ(sidebar_box.x_min, 70);
    EXPECT_TRUE(valid_box(ask_overlay_box));
    EXPECT_TRUE(valid_box(ask_scrollbar_box));
    EXPECT_TRUE(boxes_intersect(ask_overlay_box, viewport.chat_box()));
    EXPECT_TRUE(boxes_intersect(ask_scrollbar_box, ask_overlay_box));
    EXPECT_FALSE(boxes_intersect(ask_scrollbar_box, viewport.scrollbar_box()));
    EXPECT_NE(ask_scrollbar_box.x_min, viewport.scrollbar_box().x_min);
}

TEST(ChatViewport, WheelOnlyHandlesEventsInsideChatBox) {
    ChatViewport viewport;
    viewport.set_messages({
        msg("assistant", "one\ntwo\nthree\nfour\nfive\nsix\nseven"),
    });

    Screen screen(24, 4);
    Render(screen, viewport.Render());
    const int tail_top = viewport.state().scroll_top_row;

    EXPECT_FALSE(viewport.OnEvent(mouse_event(Mouse::WheelUp, Mouse::Pressed,
                                             99, 99)));
    EXPECT_EQ(viewport.state().scroll_top_row, tail_top);

    EXPECT_TRUE(viewport.OnEvent(mouse_event(Mouse::WheelUp, Mouse::Pressed,
                                            viewport.chat_box().x_min,
                                            viewport.chat_box().y_min)));
    EXPECT_LT(viewport.state().scroll_top_row, tail_top);
    EXPECT_FALSE(viewport.is_at_tail());

    EXPECT_TRUE(viewport.OnEvent(mouse_event(Mouse::WheelDown, Mouse::Pressed,
                                            viewport.chat_box().x_min,
                                            viewport.chat_box().y_min)));
}

TEST(ChatViewport, LongTranscriptScrollReusesStableCachedRows) {
    ChatViewport viewport;
    constexpr int kMessageCount = 120;
    viewport.set_messages(numbered_messages(kMessageCount));

    Screen screen(60, 8);
    Render(screen, viewport.Render());
    const auto first_stats = viewport.cache_stats();
    EXPECT_EQ(first_stats.row_count_builds, kMessageCount);
    EXPECT_LT(first_stats.builds, kMessageCount);

    const int before = viewport.state().scroll_top_row;
    EXPECT_TRUE(viewport.OnEvent(mouse_event(Mouse::WheelUp, Mouse::Pressed,
                                            viewport.chat_box().x_min,
                                            viewport.chat_box().y_min)));
    EXPECT_LT(viewport.state().scroll_top_row, before);

    Render(screen, viewport.Render());
    const auto after_scroll_stats = viewport.cache_stats();
    EXPECT_EQ(after_scroll_stats.row_count_builds, first_stats.row_count_builds);
    EXPECT_LT(after_scroll_stats.builds - first_stats.builds, 5);
    EXPECT_GT(after_scroll_stats.hits, first_stats.hits);
}

TEST(ChatViewport, RepeatedRenderWithSameMessagesSkipsLayoutRefresh) {
    ChatViewport viewport;
    viewport.set_messages(numbered_messages(80));

    Screen screen(60, 8);
    Render(screen, viewport.Render());
    const auto first_stats = viewport.cache_stats();
    ASSERT_GT(first_stats.row_count_builds, 0);

    Render(screen, viewport.Render());
    const auto second_stats = viewport.cache_stats();
    EXPECT_EQ(second_stats.row_count_builds, first_stats.row_count_builds);
    EXPECT_EQ(second_stats.row_count_misses, first_stats.row_count_misses);
    EXPECT_EQ(second_stats.builds, first_stats.builds);
}

TEST(ChatViewport, ReportsVisibleMessageIndexBounds) {
    ChatViewport viewport;
    viewport.set_messages(numbered_messages(12));

    Screen screen(60, 5);
    Render(screen, viewport.Render());

    auto [first, last] = viewport.visible_message_index_bounds();
    EXPECT_GE(first, 0);
    EXPECT_GE(last, first);
    EXPECT_LT(last, 12);
}

TEST(ChatViewport, ScrollbarWidthChangeRebuildsWidthDependentRows) {
    acecode::tui::ChatViewportOptions options;
    options.scrollbar_width = 3;
    ChatViewport viewport(options);
    viewport.set_messages({
        msg("assistant", "abcdefghijklmnopqrstuvwxyz0123456789"),
    });

    Screen screen(24, 6);
    Render(screen, viewport.Render());
    const int initial_total_rows = viewport.state().total_rows;
    const auto initial_stats = viewport.cache_stats();
    ASSERT_EQ(initial_stats.builds, 1);

    options.scrollbar_width = 10;
    viewport.set_options(options);
    Render(screen, viewport.Render());

    EXPECT_LT(viewport.chat_box().x_max, 20);
    EXPECT_GT(viewport.state().total_rows, initial_total_rows);
    EXPECT_EQ(viewport.cache_stats().builds, initial_stats.builds + 1);
}

TEST(ChatViewport, WheelResponseMatchesLegacyRowScrollMathOnLongTranscript) {
    constexpr int kMessageCount = 80;
    constexpr int kRowsPerMessage = 3;
    constexpr int kViewportRows = 8;
    const auto messages = numbered_messages(kMessageCount);
    const std::vector<int> legacy_line_counts(
        kMessageCount, kRowsPerMessage);

    ChatViewport viewport;
    viewport.set_messages(messages);

    Screen screen(60, kViewportRows);
    Render(screen, viewport.Render());

    int legacy_top = acecode::tui::chat_max_scroll_top_row(
        legacy_line_counts, kMessageCount, kViewportRows);
    ASSERT_EQ(viewport.state().scroll_top_row, legacy_top);

    EXPECT_TRUE(viewport.OnEvent(mouse_event(Mouse::WheelUp, Mouse::Pressed,
                                            viewport.chat_box().x_min,
                                            viewport.chat_box().y_min)));
    legacy_top = acecode::tui::clamp_chat_scroll_top_row(
        legacy_top - 3, legacy_line_counts, kMessageCount, kViewportRows);
    EXPECT_EQ(viewport.state().scroll_top_row, legacy_top);

    EXPECT_TRUE(viewport.OnEvent(mouse_event(Mouse::WheelDown, Mouse::Pressed,
                                            viewport.chat_box().x_min,
                                            viewport.chat_box().y_min)));
    legacy_top = acecode::tui::clamp_chat_scroll_top_row(
        legacy_top + 3, legacy_line_counts, kMessageCount, kViewportRows);
    EXPECT_EQ(viewport.state().scroll_top_row, legacy_top);
}

TEST(ChatViewport, StreamingAppendFollowsTailOnlyWhenPinned) {
    ChatViewport viewport;
    viewport.set_messages({
        msg("assistant", "one\ntwo\nthree\nfour\nfive\nsix"),
    });

    Screen screen(32, 4);
    Render(screen, viewport.Render());
    ASSERT_TRUE(viewport.is_at_tail());
    const int initial_tail = viewport.state().scroll_top_row;

    viewport.set_messages({
        msg("assistant", "one\ntwo\nthree\nfour\nfive\nsix\nseven\neight"),
    });
    Render(screen, viewport.Render());
    EXPECT_TRUE(viewport.is_at_tail());
    EXPECT_GT(viewport.state().scroll_top_row, initial_tail);

    viewport.scroll_by_rows(-2);
    const int review_top = viewport.state().scroll_top_row;
    ASSERT_FALSE(viewport.is_at_tail());

    viewport.set_messages({
        msg("assistant",
            "one\ntwo\nthree\nfour\nfive\nsix\nseven\neight\nnine\nten"),
    });
    Render(screen, viewport.Render());
    EXPECT_EQ(viewport.state().scroll_top_row, review_top);
    EXPECT_FALSE(viewport.is_at_tail());
}

TEST(ChatViewport, ExplicitTailRequestBeforeReplacePinsNewTranscript) {
    ChatViewport viewport;
    viewport.set_messages(numbered_messages(20));

    Screen screen(60, 6);
    Render(screen, viewport.Render());
    ASSERT_TRUE(viewport.is_at_tail());

    viewport.scroll_by_rows(-4);
    ASSERT_FALSE(viewport.is_at_tail());

    viewport.scroll_to_tail();
    viewport.set_messages(numbered_messages(60));
    Render(screen, viewport.Render());

    const int max_top = std::max(
        0, viewport.state().total_rows - viewport.state().viewport_rows);
    EXPECT_EQ(viewport.state().scroll_top_row, max_top);
    EXPECT_TRUE(viewport.is_at_tail());
}

TEST(ChatViewport, OverlayOwnedAreaOutsideChatIsNotConsumed) {
    ChatViewport viewport;
    viewport.set_messages({
        msg("assistant", "one\ntwo\nthree\nfour\nfive\nsix\nseven"),
    });

    Screen screen(24, 4);
    Render(screen, viewport.Render());
    const int before = viewport.state().scroll_top_row;

    EXPECT_FALSE(viewport.OnEvent(mouse_event(Mouse::WheelUp, Mouse::Pressed,
                                             viewport.chat_box().x_max + 8,
                                             viewport.chat_box().y_min)));
    EXPECT_EQ(viewport.state().scroll_top_row, before);
}

TEST(ChatViewport, ContentDragIsLeftForFtxuiSelection) {
    ChatViewport viewport;
    viewport.set_messages({msg("assistant", "selectable")});

    Screen screen(24, 3);
    Render(screen, viewport.Render());

    EXPECT_FALSE(viewport.OnEvent(mouse_event(Mouse::Left, Mouse::Pressed,
                                             viewport.chat_box().x_min,
                                             viewport.chat_box().y_min)));
}

TEST(ChatViewport, SelectionAutoscrollCanUseRowApi) {
    ChatViewport viewport;
    viewport.set_messages({
        msg("assistant", "one\ntwo\nthree\nfour\nfive\nsix\nseven\neight"),
    });

    Screen screen(24, 4);
    Render(screen, viewport.Render());
    const int tail_top = viewport.state().scroll_top_row;

    EXPECT_EQ(viewport.scroll_by_rows(-1), -1);
    EXPECT_EQ(viewport.state().scroll_top_row, tail_top - 1);
    EXPECT_FALSE(viewport.is_at_tail());

    EXPECT_EQ(viewport.scroll_by_rows(1), 1);
    EXPECT_EQ(viewport.state().scroll_top_row, tail_top);
    EXPECT_TRUE(viewport.is_at_tail());
}

TEST(ChatViewport, ScrollbarPressConsumesMouseEvent) {
    ChatViewport viewport;
    viewport.set_messages({
        msg("assistant", "one\ntwo\nthree\nfour\nfive\nsix\nseven"),
    });

    Screen screen(24, 4);
    Render(screen, viewport.Render());

    EXPECT_TRUE(viewport.OnEvent(mouse_event(Mouse::Left, Mouse::Pressed,
                                            viewport.scrollbar_box().x_min,
                                            viewport.scrollbar_box().y_min)));
}

TEST(ChatViewport, LongTranscriptScrollbarCanTargetTopMiddleAndBottom) {
    ChatViewport viewport;
    viewport.set_messages(numbered_messages(50));

    Screen screen(72, 10);
    Render(screen, viewport.Render());
    const int max_top = viewport.state().total_rows -
        viewport.state().viewport_rows;
    ASSERT_GT(max_top, 20);

    const int x = viewport.scrollbar_box().x_max;
    EXPECT_TRUE(viewport.OnEvent(mouse_event(Mouse::Left, Mouse::Pressed,
                                            x, viewport.scrollbar_box().y_min)));
    EXPECT_EQ(viewport.state().scroll_top_row, 0);
    EXPECT_FALSE(viewport.is_at_tail());

    EXPECT_TRUE(viewport.OnEvent(mouse_event(
        Mouse::None, Mouse::Moved, x,
        (viewport.scrollbar_box().y_min + viewport.scrollbar_box().y_max) / 2)));
    EXPECT_GT(viewport.state().scroll_top_row, 0);
    EXPECT_LT(viewport.state().scroll_top_row, max_top);

    EXPECT_TRUE(viewport.OnEvent(mouse_event(Mouse::None, Mouse::Moved,
                                            x, viewport.scrollbar_box().y_max)));
    EXPECT_EQ(viewport.state().scroll_top_row, max_top);
    EXPECT_TRUE(viewport.is_at_tail());

    EXPECT_TRUE(viewport.OnEvent(mouse_event(Mouse::Left, Mouse::Released,
                                            x, viewport.scrollbar_box().y_max)));
}

TEST(ChatViewport, PageAndAltKeysUseRowScrollModel) {
    ChatViewport viewport;
    viewport.set_messages({
        msg("assistant", "one\ntwo\nthree\nfour\nfive\nsix\nseven\neight"),
    });

    Screen screen(24, 4);
    Render(screen, viewport.Render());
    const int tail_top = viewport.state().scroll_top_row;

    EXPECT_TRUE(viewport.OnEvent(Event::PageUp));
    EXPECT_EQ(viewport.state().scroll_top_row, tail_top - 2);
    EXPECT_TRUE(viewport.OnEvent(Event::Special("\x1B[1;3B")));
    EXPECT_EQ(viewport.state().scroll_top_row, tail_top - 1);
    EXPECT_TRUE(viewport.OnEvent(Event::PageDown));
    EXPECT_EQ(viewport.state().scroll_top_row, tail_top);
    EXPECT_TRUE(viewport.is_at_tail());
}

TEST(ChatViewport, KeyEventsAreIgnoredBeforeLayoutBoxExists) {
    ChatViewport viewport;
    viewport.set_messages({
        msg("assistant", "one\ntwo\nthree\nfour\nfive\nsix\nseven\neight"),
    });

    EXPECT_FALSE(viewport.OnEvent(Event::PageUp));
    EXPECT_FALSE(viewport.OnEvent(Event::PageDown));
    EXPECT_FALSE(viewport.OnEvent(Event::Special("\x1B[1;3A")));
    EXPECT_EQ(viewport.state().viewport_rows, 0);
    EXPECT_EQ(viewport.state().scroll_top_row, 0);
}

TEST(ChatViewport, ResizeRebuildClampsReviewPosition) {
    ChatViewport viewport;
    viewport.set_messages({
        msg("assistant", "one\ntwo\nthree\nfour\nfive\nsix\nseven\neight"),
    });

    Screen small(24, 4);
    Render(small, viewport.Render());
    ASSERT_GT(viewport.state().scroll_top_row, 0);
    viewport.scroll_by_rows(-1);
    ASSERT_FALSE(viewport.is_at_tail());

    Screen tall(24, 12);
    Render(tall, viewport.Render());
    EXPECT_EQ(viewport.state().scroll_top_row, 0);
    EXPECT_TRUE(viewport.is_at_tail());
}

TEST(ChatViewport, ToolResultExpansionInvalidatesOnlyChangedMessage) {
    ChatViewportMessageInput stable = msg("assistant", "stable history");
    ChatViewportMessageInput tool = msg("tool_result", "summary row");
    tool.has_summary = true;

    ChatViewport viewport;
    viewport.set_messages({stable, tool});

    Screen screen(48, 5);
    Render(screen, viewport.Render());
    const auto first_stats = viewport.cache_stats();
    ASSERT_EQ(first_stats.builds, 2);

    tool.expanded = true;
    tool.content = "summary row\nexpanded detail\nmore detail";
    viewport.set_messages({stable, tool});
    Render(screen, viewport.Render());

    const auto expanded_stats = viewport.cache_stats();
    EXPECT_EQ(expanded_stats.builds, 3);
    EXPECT_GT(expanded_stats.row_count_hits, first_stats.row_count_hits);
    EXPECT_LE(viewport.state().scroll_top_row,
              std::max(0, viewport.state().total_rows -
                           viewport.state().viewport_rows));
}

TEST(ChatViewport, ScrollbarDragUsesSnapshotDuringStreamingAppend) {
    ChatViewport viewport;
    viewport.set_messages({
        msg("assistant", "one\ntwo\nthree\nfour\nfive\nsix\nseven\neight"),
    });

    Screen screen(24, 4);
    Render(screen, viewport.Render());
    const int old_max_top = viewport.state().scroll_top_row;

    EXPECT_TRUE(viewport.OnEvent(mouse_event(Mouse::Left, Mouse::Pressed,
                                            viewport.scrollbar_box().x_max,
                                            viewport.scrollbar_box().y_min)));

    viewport.set_messages({
        msg("assistant", "one\ntwo\nthree\nfour\nfive\nsix\nseven\neight"
                         "\nnine\nten\neleven\ntwelve"),
    });
    Render(screen, viewport.Render());
    const int new_max_top = viewport.state().total_rows -
        viewport.state().viewport_rows;
    ASSERT_GT(new_max_top, old_max_top);

    EXPECT_TRUE(viewport.OnEvent(mouse_event(Mouse::None, Mouse::Moved,
                                            viewport.scrollbar_box().x_max,
                                            viewport.scrollbar_box().y_max)));
    EXPECT_EQ(viewport.state().scroll_top_row, old_max_top);
    EXPECT_LT(viewport.state().scroll_top_row, new_max_top);

    EXPECT_TRUE(viewport.OnEvent(mouse_event(Mouse::Left, Mouse::Released,
                                            viewport.scrollbar_box().x_max,
                                            viewport.scrollbar_box().y_max)));
}
