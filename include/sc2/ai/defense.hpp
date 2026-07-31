#pragma once

#include <cstdint>

#include "sc2/sim/world.hpp"

namespace sc2::ai {

inline std::uint32_t nearest_owned_base(const sim::World& world, int32_t player, sim::Fixed x,
                                        sim::Fixed y) {
    using sim::Fixed;
    std::uint32_t best = sim::kNoTarget;
    Fixed best_d2;
    for (const sim::Unit& u : world.units()) {
        if (!u.alive || u.player != player || u.speed.raw() != 0) continue;
        const Fixed dx = u.x - x, dy = u.y - y;
        const Fixed d2 = dx * dx + dy * dy;
        if (best == sim::kNoTarget || d2 < best_d2) {
            best = u.id;
            best_d2 = d2;
        }
    }
    return best;
}

inline std::uint32_t enemy_near(const sim::World& world, int32_t player, sim::Fixed x,
                                sim::Fixed y, sim::Fixed radius) {
    using sim::Fixed;
    const Fixed r2 = radius * radius;
    std::uint32_t best = sim::kNoTarget;
    Fixed best_d2;
    for (const sim::Unit& e : world.units()) {
        if (!e.alive || sim::World::is_neutral(e.player)) continue;
        if (world.are_allied(e.player, player)) continue;
        const Fixed dx = e.x - x, dy = e.y - y;
        const Fixed d2 = dx * dx + dy * dy;
        if (d2 > r2) continue;
        if (best == sim::kNoTarget || d2 < best_d2) {
            best = e.id;
            best_d2 = d2;
        }
    }
    return best;
}

inline std::uint32_t threatened_base(const sim::World& world, int32_t player, sim::Fixed radius,
                                     std::uint32_t& enemy_out) {
    enemy_out = sim::kNoTarget;
    for (const sim::Unit& u : world.units()) {
        if (!u.alive || u.player != player || u.speed.raw() != 0) continue;
        const std::uint32_t e = enemy_near(world, player, u.x, u.y, radius);
        if (e != sim::kNoTarget) {
            enemy_out = e;
            return u.id;
        }
    }
    return sim::kNoTarget;
}

}
