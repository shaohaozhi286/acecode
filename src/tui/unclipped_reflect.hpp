#pragma once

#include <memory>
#include <utility>

#include "ftxui/dom/elements.hpp"
#include "ftxui/dom/node.hpp"
#include "ftxui/screen/box.hpp"
#include "ftxui/screen/screen.hpp"

namespace acecode::tui {

class UnclippedReflect : public ftxui::Node {
 public:
    UnclippedReflect(ftxui::Element child, ftxui::Box& box)
        : ftxui::Node(ftxui::unpack(std::move(child))), reflected_box_(box) {}

    void ComputeRequirement() final {
        ftxui::Node::ComputeRequirement();
        requirement_ = children_[0]->requirement();
    }

    void SetBox(ftxui::Box box) final {
        reflected_box_ = box;
        ftxui::Node::SetBox(box);
        children_[0]->SetBox(box);
    }

    void Render(ftxui::Screen& screen) final {
        ftxui::Node::Render(screen);
    }

 private:
    ftxui::Box& reflected_box_;
};

inline ftxui::Decorator reflect_unclipped(ftxui::Box& box) {
    return [&](ftxui::Element child) -> ftxui::Element {
        return std::make_shared<UnclippedReflect>(std::move(child), box);
    };
}

} // namespace acecode::tui
