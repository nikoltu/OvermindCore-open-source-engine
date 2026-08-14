#pragma once

#include "sc2/app/ui_canvas.hpp"
#include "sc2/ui/layout.hpp"

namespace sc2::app {

inline ui::Rect canvas_solve_rect(const Canvas& c) {
    return ui::Rect{0.0f, 0.0f, c.virt_w, c.virt_h};
}

inline ui::Rect solved_to_physical(const Canvas& c, const ui::Rect& solved) {
    return ui::Rect{solved.x * c.scale, solved.y * c.scale, solved.w * c.scale, solved.h * c.scale};
}

inline void physical_point_to_virtual(const Canvas& c, float px, float py, float& vx, float& vy) {
    vx = phys_to_virt(c, px);
    vy = phys_to_virt(c, py);
}

}
