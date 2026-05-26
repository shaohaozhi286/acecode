#include <gtest/gtest.h>

#include <ftxui/dom/elements.hpp>
#include <ftxui/dom/node.hpp>
#include <ftxui/dom/selection.hpp>
#include <ftxui/screen/screen.hpp>

#include "tui/non_selectable.hpp"

#include <string>

using acecode::tui::non_selectable;
using ftxui::Element;
using ftxui::Render;
using ftxui::Screen;
using ftxui::Selection;
using ftxui::hbox;
using ftxui::text;

TEST(NonSelectableElement, ExcludesWrappedTextFromSelectionParts) {
    Element element = hbox({
        text("main"),
        non_selectable(text("side")),
    });

    auto screen = Screen::Create(ftxui::Dimension::Fixed(12),
                                 ftxui::Dimension::Fixed(1));
    Selection selection(0, 0, 7, 0);
    Render(screen, element.get(), selection);

    EXPECT_EQ(selection.GetParts(), "main");
}

TEST(NonSelectableElement, DoesNotRenderSelectionHighlightInsideWrappedText) {
    Element element = hbox({
        text("main"),
        non_selectable(text("side")),
    });

    auto screen = Screen::Create(ftxui::Dimension::Fixed(12),
                                 ftxui::Dimension::Fixed(1));
    Selection selection(0, 0, 7, 0);
    Render(screen, element.get(), selection);

    const std::string rendered = screen.ToString();
    EXPECT_NE(rendered.find("\x1B[7mmain\x1B[27mside"), std::string::npos)
        << rendered;
}

TEST(NonSelectableElement, SelectionInsideWrappedTextProducesNoContent) {
    Element element = hbox({
        text("main"),
        non_selectable(text("side")),
    });

    auto screen = Screen::Create(ftxui::Dimension::Fixed(12),
                                 ftxui::Dimension::Fixed(1));
    Selection selection(4, 0, 7, 0);
    Render(screen, element.get(), selection);

    EXPECT_TRUE(selection.GetParts().empty());
    EXPECT_EQ(screen.ToString(), "mainside    ");
}
