#pragma once

#include <cstdint>
#include <vector>

#include "sc2/sim/fixed.hpp"
#include "sc2/sim/world.hpp"

namespace sc2::sim {

class NovaSystem {
public:
    int launch(Fixed x, Fixed y, Fixed tx, Fixed ty, Fixed speed, Fixed radius, Fixed damage,
               int32_t owner, Fixed max_life) {
        Nova n;
        n.x = x;
        n.y = y;
        n.tx = tx;
        n.ty = ty;
        n.speed = speed;
        n.radius = radius;
        n.damage = damage;
        n.owner = owner;
        n.remaining = max_life.raw() > 0 ? max_life : Fixed::from_int(10);
        n.active = true;
        novas_.push_back(n);
        return int(novas_.size()) - 1;
    }

    int active_count() const {
        int k = 0;
        for (const Nova& n : novas_)
            if (n.active) ++k;
        return k;
    }
    bool position(int handle, Fixed& x, Fixed& y) const {
        if (handle < 0 || std::size_t(handle) >= novas_.size() || !novas_[handle].active)
            return false;
        x = novas_[handle].x;
        y = novas_[handle].y;
        return true;
    }

    int advance(World& world, Fixed dt) {
        int detonations = 0;
        for (Nova& n : novas_) {
            if (!n.active) continue;
            n.remaining = n.remaining - dt;

            const Fixed dx = n.tx - n.x, dy = n.ty - n.y;
            const Fixed dist = isqrt(dx * dx + dy * dy);
            const Fixed step = n.speed * dt;
            bool arrived = false;
            if (dist <= step || dist.raw() <= 0) {
                n.x = n.tx;
                n.y = n.ty;
                arrived = true;
            } else {
                n.x = n.x + dx * step / dist;
                n.y = n.y + dy * step / dist;
            }

            if (arrived || n.remaining.raw() <= 0) {
                detonate(world, n);
                n.active = false;
                ++detonations;
            }
        }
        return detonations;
    }

private:
    struct Nova {
        Fixed x, y, tx, ty;
        Fixed speed, radius, damage;
        int32_t owner = -1;
        Fixed remaining;
        bool active = false;
    };

    static void detonate(World& world, const Nova& n) {
        const Fixed r2 = n.radius * n.radius;
        const std::uint32_t k = std::uint32_t(world.units().size());
        for (std::uint32_t i = 0; i < k; ++i) {
            Unit& u = world.unit(i);
            if (!u.alive || world.are_allied(u.player, n.owner)) continue;
            const Fixed dx = u.x - n.x, dy = u.y - n.y;
            if (dx * dx + dy * dy > r2) continue;
            Fixed dmg = n.damage;
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

    std::vector<Nova> novas_;
};

}
