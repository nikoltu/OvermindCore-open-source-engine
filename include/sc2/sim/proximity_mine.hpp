#pragma once

#include <cstdint>
#include <vector>

#include "sc2/sim/world.hpp"

namespace sc2::sim {

class ProximityMineSystem {
public:
    int arm(std::int32_t owner, Fixed x, Fixed y, Fixed trigger_radius, Fixed blast_radius,
            Fixed damage, Fixed arm_delay = Fixed()) {
        Mine m;
        m.owner = owner;
        m.x = x;
        m.y = y;
        m.trigger_r = trigger_radius;
        m.blast_r = blast_radius;
        m.damage = damage;
        m.arm_timer = arm_delay.raw() > 0 ? arm_delay : Fixed();
        m.live = true;
        mines_.push_back(m);
        return int(mines_.size()) - 1;
    }

    int live_count() const {
        int n = 0;
        for (const Mine& m : mines_)
            if (m.live) ++n;
        return n;
    }

    int advance(World& world, Fixed dt) {
        int detonations = 0;
        const std::uint32_t n = std::uint32_t(world.units().size());
        for (Mine& m : mines_) {
            if (!m.live) continue;
            if (m.arm_timer.raw() > 0) {
                m.arm_timer = m.arm_timer - dt;
                continue;
            }
            const Fixed tr2 = m.trigger_r * m.trigger_r;
            bool triggered = false;
            for (std::uint32_t i = 0; i < n && !triggered; ++i) {
                const Unit& u = world.units()[i];
                if (!u.alive || world.are_allied(u.player, m.owner)) continue;
                const Fixed dx = u.x - m.x, dy = u.y - m.y;
                if (dx * dx + dy * dy <= tr2) triggered = true;
            }
            if (!triggered) continue;

            const Fixed br2 = m.blast_r * m.blast_r;
            for (std::uint32_t i = 0; i < n; ++i) {
                Unit& u = world.unit(i);
                if (!u.alive || world.are_allied(u.player, m.owner)) continue;
                const Fixed dx = u.x - m.x, dy = u.y - m.y;
                if (dx * dx + dy * dy > br2) continue;
                Fixed dmg = m.damage;
                if (u.shields.raw() > 0) {
                    if (u.shields >= dmg) {
                        u.shields = u.shields - dmg;
                        continue;
                    }
                    dmg = dmg - u.shields;
                    u.shields = Fixed();
                }
                u.hp = u.hp - dmg;
                if (u.hp.raw() <= 0) u.alive = false;
            }
            m.live = false;
            ++detonations;
        }
        return detonations;
    }

private:
    struct Mine {
        std::int32_t owner = -1;
        Fixed x, y;
        Fixed trigger_r, blast_r, damage;
        Fixed arm_timer;
        bool live = false;
    };
    std::vector<Mine> mines_;
};

}
