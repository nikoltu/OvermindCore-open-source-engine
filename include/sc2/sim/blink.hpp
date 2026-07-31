#pragma once

#include <cstdint>
#include <vector>

#include "sc2/sim/world.hpp"

namespace sc2::sim {

inline bool blink(World& world, std::uint32_t id, Fixed tx, Fixed ty, Fixed max_range) {
    if (id >= world.units().size()) return false;
    Unit& u = world.unit(id);
    if (!u.alive) return false;
    if (max_range.raw() > 0) {
        const Fixed dx = tx - u.x, dy = ty - u.y;
        const Fixed r2 = max_range * max_range;
        if (dx * dx + dy * dy > r2) return false;
    }
    u.x = tx;
    u.y = ty;
    u.path.clear();
    u.path_next = 0;
    return true;
}

inline int recall(World& world, std::int32_t owner, Fixed cx, Fixed cy, Fixed gather_radius,
                  Fixed tx, Fixed ty) {
    const std::uint32_t n = std::uint32_t(world.units().size());
    const Fixed r2 = gather_radius * gather_radius;
    std::vector<std::uint32_t> picked;
    for (std::uint32_t i = 0; i < n; ++i) {
        const Unit& u = world.units()[i];
        if (!u.alive) continue;
        if (!world.are_allied(u.player, owner)) continue;
        const Fixed dx = u.x - cx, dy = u.y - cy;
        if (dx * dx + dy * dy > r2) continue;
        picked.push_back(i);
    }
    static const int DIR[8][2] = {{0, 0}, {1, 0}, {0, 1}, {-1, 0},
                                  {0, -1}, {1, 1}, {-1, 1}, {1, -1}};
    const Fixed step = Fixed::from_int(1);
    for (std::size_t k = 0; k < picked.size(); ++k) {
        Unit& u = world.unit(picked[k]);
        const int d = int(k % 8);
        const int ring = int(k / 8) + 1;
        u.x = tx + step * Fixed::from_int(DIR[d][0] * (d == 0 ? 0 : ring));
        u.y = ty + step * Fixed::from_int(DIR[d][1] * (d == 0 ? 0 : ring));
        u.path.clear();
        u.path_next = 0;
    }
    return int(picked.size());
}

}
