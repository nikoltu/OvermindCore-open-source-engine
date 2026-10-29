#pragma once

#include <cstdint>
#include <vector>

#include "omc/sim/world.hpp"

namespace omc::ai {

inline bool& focus_skips_neutral() {
    static bool on = false;
    return on;
}

inline std::uint32_t focus_target(const sim::World& world, const std::vector<std::uint32_t>& group,
                                  int32_t player) {
    using sim::Fixed;
    std::uint32_t best = sim::kNoTarget;
    Fixed best_ehp;
    const auto& units = world.units();
    const bool skip_neutral = focus_skips_neutral();
    for (const sim::Unit& e : units) {
        if (!e.alive || world.are_allied(e.player, player)) continue;
        if (skip_neutral && sim::World::is_neutral(e.player)) continue;
        if (e.loaded || e.burrowed) continue;

        bool reachable = false;
        for (std::uint32_t gid : group) {
            if (gid >= units.size()) continue;
            const sim::Unit& g = units[gid];
            if (!g.alive || g.player != player) continue;
            if (e.flying ? !(g.wpn_target_filter & 2u) : !(g.wpn_target_filter & 1u)) continue;
            const Fixed dx = e.x - g.x, dy = e.y - g.y;
            const Fixed reach = g.wpn_range + g.radius + e.radius;
            if (dx * dx + dy * dy <= reach * reach) {
                reachable = true;
                break;
            }
        }
        if (!reachable) continue;

        const Fixed ehp = e.hp + e.shields;
        if (best == sim::kNoTarget || ehp < best_ehp || (ehp == best_ehp && e.id < best)) {
            best = e.id;
            best_ehp = ehp;
        }
    }
    return best;
}

inline std::uint32_t command_focus_fire(sim::World& world, const std::vector<std::uint32_t>& group,
                                        int32_t player) {
    const std::uint32_t target = focus_target(world, group, player);
    if (target == sim::kNoTarget) return sim::kNoTarget;
    sim::Order o;
    o.type = sim::OrderType::Attack;
    o.target = target;
    for (std::uint32_t gid : group) {
        if (gid >= world.units().size()) continue;
        const sim::Unit& g = world.units()[gid];
        if (g.alive && g.player == player) world.issue(gid, o);
    }
    return target;
}

enum class MicroAction {
    Idle,
    Advance,
    Attack,
    Kite,
    Retreat,
};

inline MicroAction micro_decide(const sim::World& world, std::uint32_t unit, std::uint32_t enemy,
                                sim::Fixed retreat_hp_frac) {
    using sim::Fixed;
    const auto& units = world.units();
    if (unit >= units.size() || enemy >= units.size()) return MicroAction::Idle;
    const sim::Unit& u = units[unit];
    const sim::Unit& e = units[enemy];
    if (!u.alive || !e.alive) return MicroAction::Idle;

    if (u.hp_max.raw() > 0 && u.hp < u.hp_max * retreat_hp_frac) return MicroAction::Retreat;

    const Fixed dx = e.x - u.x, dy = e.y - u.y;
    const Fixed d2 = dx * dx + dy * dy;
    const Fixed my_reach = u.wpn_range + u.radius + e.radius;
    const Fixed their_reach = e.wpn_range + e.radius + u.radius;

    if (d2 > my_reach * my_reach) return MicroAction::Advance;
    if (u.wpn_range > e.wpn_range && d2 <= their_reach * their_reach) return MicroAction::Kite;
    return MicroAction::Attack;
}

inline void command_micro(sim::World& world, std::uint32_t unit, std::uint32_t enemy,
                          MicroAction action, sim::Fixed rally_x, sim::Fixed rally_y) {
    if (unit >= world.units().size()) return;
    sim::Order o;
    switch (action) {
        case MicroAction::Attack:
        case MicroAction::Advance:
            o.type = sim::OrderType::Attack;
            o.target = enemy;
            break;
        case MicroAction::Kite:
        case MicroAction::Retreat:
            o.type = sim::OrderType::Move;
            o.tx = rally_x;
            o.ty = rally_y;
            break;
        case MicroAction::Idle:
        default:
            return;
    }
    world.issue(unit, o);
}

}
