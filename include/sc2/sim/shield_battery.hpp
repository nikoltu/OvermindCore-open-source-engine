#pragma once

#include <cstdint>
#include <vector>

#include "sc2/sim/world.hpp"

namespace sc2::sim {

class ShieldBatterySystem {
public:
    int add(std::uint32_t source, Fixed radius, Fixed restore_per_sec, Fixed energy_per_shield) {
        batteries_.push_back(Battery{source, radius, restore_per_sec, energy_per_shield});
        return int(batteries_.size()) - 1;
    }

    void advance(World& world, Fixed dt) {
        const std::uint32_t n = std::uint32_t(world.units().size());
        for (const Battery& b : batteries_) {
            if (b.source >= n) continue;
            Unit& src = world.unit(b.source);
            if (!src.alive || src.energy.raw() <= 0 || b.radius.raw() <= 0) continue;
            const Fixed sx = src.x, sy = src.y;
            const int32_t sp = src.player;
            const Fixed r2 = b.radius * b.radius;
            Fixed budget = b.restore_per_sec * dt;

            for (std::uint32_t i = 0; i < n && budget.raw() > 0 && src.energy.raw() > 0; ++i) {
                if (i == b.source) continue;
                Unit& t = world.unit(i);
                if (!t.alive || t.shields_max.raw() <= 0) continue;
                if (t.shields >= t.shields_max) continue;
                if (!world.are_allied(t.player, sp)) continue;
                const Fixed dx = t.x - sx, dy = t.y - sy;
                if (dx * dx + dy * dy > r2) continue;

                Fixed want = t.shields_max - t.shields;
                if (want > budget) want = budget;
                if (b.energy_per_shield.raw() > 0) {
                    Fixed affordable = src.energy / b.energy_per_shield;
                    if (want > affordable) want = affordable;
                }
                if (want.raw() <= 0) continue;

                t.shields = t.shields + want;
                budget = budget - want;
                if (b.energy_per_shield.raw() > 0)
                    src.energy = src.energy - want * b.energy_per_shield;
                if (src.energy.raw() < 0) src.energy = Fixed();
            }
        }
    }

private:
    struct Battery {
        std::uint32_t source = kNoTarget;
        Fixed radius;
        Fixed restore_per_sec;
        Fixed energy_per_shield;
    };
    std::vector<Battery> batteries_;
};

}
