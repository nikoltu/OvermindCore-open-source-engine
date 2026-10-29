#pragma once

#include <cstdint>
#include <vector>

#include "omc/sim/world.hpp"

namespace omc::ai {

inline sim::Fixed unit_power(const sim::Unit& u) {
    using sim::Fixed;
    const Fixed ehp = u.hp + u.shields;
    if (ehp.raw() <= 0) return Fixed();
    if (u.wpn_damage.raw() <= 0 || u.wpn_period.raw() <= 0) return ehp;
    const Fixed dps = u.wpn_damage / u.wpn_period;
    return ehp + ehp * dps;
}

inline sim::Fixed army_power(const sim::World& world, int32_t player, sim::Fixed cx = sim::Fixed(),
                             sim::Fixed cy = sim::Fixed(), sim::Fixed radius = sim::Fixed()) {
    using sim::Fixed;
    Fixed total;
    const bool local = radius.raw() > 0;
    const Fixed r2 = radius * radius;
    for (const sim::Unit& u : world.units()) {
        if (!u.alive || u.player != player) continue;
        if (u.speed.raw() == 0) continue;
        if (local) {
            const Fixed dx = u.x - cx, dy = u.y - cy;
            if (dx * dx + dy * dy > r2) continue;
        }
        total = total + unit_power(u);
    }
    return total;
}

inline bool should_engage(sim::Fixed my_power, sim::Fixed enemy_power, sim::Fixed caution) {
    if (enemy_power.raw() <= 0) return true;
    return my_power >= enemy_power * caution;
}

inline bool should_engage_local(const sim::World& world, int32_t player, int32_t enemy,
                                sim::Fixed cx, sim::Fixed cy, sim::Fixed radius,
                                sim::Fixed caution) {
    return should_engage(army_power(world, player, cx, cy, radius),
                         army_power(world, enemy, cx, cy, radius), caution);
}

}
