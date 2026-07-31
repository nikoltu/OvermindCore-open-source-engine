#pragma once

#include <cstdint>
#include <vector>

#include "sc2/ai/assess.hpp"
#include "sc2/ai/micro.hpp"
#include "sc2/sim/world.hpp"

namespace sc2::ai {

class CombatController {
public:
    sim::Fixed caution = sim::Fixed::from_int(1);
    sim::Fixed retreat_hp_frac = sim::Fixed::from_ratio(1, 3);
    sim::Fixed engage_radius = sim::Fixed::from_int(12);

    enum class Stance { Idle, Engage, Retreat };

    Stance command(sim::World& world, const std::vector<std::uint32_t>& group, int32_t player,
                   sim::Fixed rally_x, sim::Fixed rally_y) {
        using sim::Fixed;
        Fixed cx, cy;
        int n = 0;
        for (std::uint32_t id : group)
            if (id < world.units().size()) {
                const sim::Unit& u = world.units()[id];
                if (u.alive && u.player == player) {
                    cx = cx + u.x;
                    cy = cy + u.y;
                    ++n;
                }
            }
        if (n == 0) return Stance::Idle;
        cx = cx / Fixed::from_int(n);
        cy = cy / Fixed::from_int(n);

        const std::uint32_t focus = focus_target(world, group, player);
        if (focus == sim::kNoTarget) return Stance::Idle;
        const int32_t enemy_player = world.units()[focus].player;

        const Fixed mine = army_power(world, player, cx, cy, engage_radius);
        const Fixed theirs = army_power(world, enemy_player, cx, cy, engage_radius);
        if (!should_engage(mine, theirs, caution)) {
            for (std::uint32_t id : group)
                command_micro(world, id, focus, MicroAction::Retreat, rally_x, rally_y);
            return Stance::Retreat;
        }

        for (std::uint32_t id : group) {
            if (id >= world.units().size()) continue;
            const sim::Unit& u = world.units()[id];
            if (!u.alive || u.player != player) continue;
            MicroAction a = micro_decide(world, id, focus, retreat_hp_frac);
            command_micro(world, id, focus, a, rally_x, rally_y);
        }
        return Stance::Engage;
    }
};

}
