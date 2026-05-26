#pragma once

#include <memory>
#include <string>
#include <utility>

#include <ftxui/dom/elements.hpp>
#include <ftxui/dom/node.hpp>
#include <ftxui/dom/selection.hpp>
#include <ftxui/screen/box.hpp>

namespace acecode::tui {

namespace detail {

class NonSelectableElement final : public ftxui::Node {
 public:
    explicit NonSelectableElement(ftxui::Element child)
        : ftxui::Node(ftxui::unpack(std::move(child))) {}

    void ComputeRequirement() final {
        ftxui::Node::ComputeRequirement();
        requirement_ = children_[0]->requirement();
    }

    void SetBox(ftxui::Box box) final {
        ftxui::Node::SetBox(box);
        children_[0]->SetBox(box);
    }

    void Select(ftxui::Selection&) final {}

    std::string GetSelectedContent(ftxui::Selection&) final {
        return {};
    }
};

} // namespace detail

inline ftxui::Element non_selectable(ftxui::Element child) {
    return std::make_shared<detail::NonSelectableElement>(std::move(child));
}

} // namespace acecode::tui
