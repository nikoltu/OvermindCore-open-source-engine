#pragma once

#include <cstdint>
#include <vector>

#include "omc/sim/world.hpp"

namespace omc::sim {

class NukeSystem {
public:
    int launch(std::int32_t owner, Fixed x, Fixed y, Fixed radius, Fixed damage, Fixed delay) {
        Strike s;
        s.owner = owner;
        s.x = x;
        s.y = y;
        s.radius = radius;
        s.damage = damage;
        s.timer = delay.raw() > 0 ? delay : Fixed::from_raw(1);
        s.pending = true;
        strikes_.push_back(s);
        return int(strikes_.size()) - 1;
    }

    int pending_count() const {
        int n = 0;
        for (const Strike& s : strikes_)
            if (s.pending) ++n;
        return n;
    }
    Fixed time_remaining(int handle) const {
        if (handle < 0 || std::size_t(handle) >= strikes_.size()) return Fixed();
        const Strike& s = strikes_[handle];
        return s.pending ? s.timer : Fixed();
    }

    int advance(World& world, Fixed dt) {
        int booms = 0;
        for (Strike& s : strikes_) {
            if (!s.pending) continue;
            s.timer = s.timer - dt;
            if (s.timer.raw() > 0) continue;
            s.pending = false;
            ++booms;

            const Fixed r2 = s.radius * s.radius;
            const std::uint32_t n = std::uint32_t(world.units().size());
            for (std::uint32_t i = 0; i < n; ++i) {
                Unit& u = world.unit(i);
                if (!u.alive || world.are_allied(u.player, s.owner)) continue;
                const Fixed dx = u.x - s.x, dy = u.y - s.y;
                if (dx * dx + dy * dy > r2) continue;
                Fixed dmg = s.damage;
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
        }
        return booms;
    }

private:
    struct Strike {
        std::int32_t owner = -1;
        Fixed x, y, radius, damage, timer;
        bool pending = false;
    };
    std::vector<Strike> strikes_;
};

}
