#pragma once

#include <cstdint>

#include "sc2/sim/world.hpp"

namespace sc2::sim {

struct EmpResult {
    int units_hit = 0;
    Fixed shields_removed;
    Fixed energy_removed;
};

inline EmpResult emp_strike(World& world, Fixed x, Fixed y, Fixed radius, Fixed energy_drain,
                            std::int32_t caster_player, bool enemies_only = true) {
    EmpResult r;
    if (radius.raw() <= 0) return r;
    const Fixed r2 = radius * radius;
    const std::uint32_t n = std::uint32_t(world.units().size());
    for (std::uint32_t i = 0; i < n; ++i) {
        Unit& u = world.unit(i);
        if (!u.alive) continue;
        if (enemies_only && world.are_allied(u.player, caster_player)) continue;
        const Fixed dx = u.x - x, dy = u.y - y;
        if (dx * dx + dy * dy > r2) continue;

        bool touched = false;
        if (u.shields.raw() > 0) {
            r.shields_removed = r.shields_removed + u.shields;
            u.shields = Fixed();
            touched = true;
        }
        if (energy_drain.raw() > 0 && u.energy.raw() > 0) {
            Fixed drain = u.energy < energy_drain ? u.energy : energy_drain;
            u.energy = u.energy - drain;
            r.energy_removed = r.energy_removed + drain;
            touched = true;
        }
        if (touched) ++r.units_hit;
    }
    return r;
}

}
