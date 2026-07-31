#pragma once

#include <cstdint>

#include "sc2/sim/vision.hpp"

namespace sc2::ai {

inline bool scout_target(const sim::VisionGrid& vision, const sim::Bounds& bounds,
                         sim::Fixed cell_size, sim::Fixed from_x, sim::Fixed from_y,
                         sim::Fixed& out_x, sim::Fixed& out_y) {
    using sim::Fixed;
    const Fixed half = Fixed::from_raw(cell_size.raw() / 2);
    bool found = false;
    std::int64_t best_d2 = 0;
    for (int cy = 0; cy < vision.height(); ++cy) {
        const Fixed wy = bounds.min_y + cell_size * Fixed::from_int(cy) + half;
        for (int cx = 0; cx < vision.width(); ++cx) {
            if (vision.state(cx, cy) != sim::Visibility::Hidden) continue;
            const Fixed wx = bounds.min_x + cell_size * Fixed::from_int(cx) + half;
            const std::int64_t dx = std::int64_t((wx - from_x).raw());
            const std::int64_t dy = std::int64_t((wy - from_y).raw());
            const std::int64_t d2 = dx * dx + dy * dy;
            if (!found || d2 < best_d2) {
                found = true;
                best_d2 = d2;
                out_x = wx;
                out_y = wy;
            }
        }
    }
    return found;
}

inline sim::Fixed unexplored_fraction(const sim::VisionGrid& vision) {
    const int total = vision.width() * vision.height();
    if (total <= 0) return sim::Fixed();
    int hidden = 0;
    for (int cy = 0; cy < vision.height(); ++cy)
        for (int cx = 0; cx < vision.width(); ++cx)
            if (vision.state(cx, cy) == sim::Visibility::Hidden) ++hidden;
    return sim::Fixed::from_ratio(hidden, total);
}

}
