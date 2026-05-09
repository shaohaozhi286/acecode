// acetui/src/viewport.cpp

#include "acetui/viewport.hpp"

namespace acetui {

Viewport Viewport::bottom(Size screen, int height) {
    Viewport vp;
    vp.left   = 1;
    vp.width  = screen.width  > 0 ? screen.width  : 1;
    vp.height = height        > 0 ? height        : 1;

    int top = screen.height - vp.height + 1;
    vp.top  = top < 1 ? 1 : top;
    return vp;
}

}  // namespace acetui
