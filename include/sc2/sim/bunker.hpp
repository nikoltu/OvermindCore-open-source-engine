#pragma once

#include <cstdint>
#include <vector>

#include "sc2/sim/combat.hpp"
#include "sc2/sim/transport.hpp"
#include "sc2/sim/world.hpp"

namespace sc2::sim {

class BunkerFireSystem {
public:
    int advance(World& world, const TransportBay& bay, std::uint32_t bunker, Fixed fire_range,
                Fixed dt) {
        if (bunker >= world.units().size()) return 0;
        const Unit b = world.units()[bunker];
        if (!b.alive) return 0;
        const std::int32_t owner = b.player;
        const Fixed bx = b.x, by = b.y;
        const Fixed range2 = fire_range * fire_range;

        int shots = 0;
        for (std::uint32_t pid : bay.passengers()) {
            if (pid >= world.units().size()) continue;
            Unit& p = world.unit(pid);
            if (!p.alive || p.wpn_damage.raw() <= 0) continue;

            if (p.cooldown.raw() > 0) {
                p.cooldown = p.cooldown - dt;
                if (p.cooldown.raw() < 0) p.cooldown = Fixed();
            }
            if (p.cooldown.raw() > 0) continue;

            std::uint32_t best = kNoTarget;
            Fixed best_d2;
            const std::uint32_t n = std::uint32_t(world.units().size());
            for (std::uint32_t i = 0; i < n; ++i) {
                const Unit& t = world.units()[i];
                if (!t.alive || t.loaded || world.are_allied(t.player, owner)) continue;
                const Fixed dx = t.x - bx, dy = t.y - by;
                const Fixed d2 = dx * dx + dy * dy;
                if (d2.raw() > range2.raw()) continue;
                if (best == kNoTarget || d2.raw() < best_d2.raw()) {
                    best = i;
                    best_d2 = d2;
                }
            }
            if (best == kNoTarget) continue;

            Unit& tgt = world.unit(best);
            Defender d{tgt.hp, tgt.shields, tgt.armor, tgt.damage_reduction};
            apply_hit(Weapon{p.wpn_damage, p.wpn_period}, d);
            tgt.hp = d.life;
            tgt.shields = d.shields;
            if (tgt.hp.raw() <= 0) tgt.alive = false;
            p.cooldown = p.wpn_period;
            ++shots;
        }
        return shots;
    }

private:
};

}
