#pragma once

#include <cstdint>
#include <vector>

#include "sc2/sim/world.hpp"

namespace sc2::sim {

class ChannelSystem {
public:
    int begin(const World& world, std::uint32_t caster, std::uint32_t target, Fixed cast_time,
              Fixed damage, Fixed range, Fixed hold_radius = Fixed()) {
        if (caster >= world.units().size() || target >= world.units().size()) return -1;
        const Unit& c = world.units()[caster];
        Channel ch;
        ch.caster = caster;
        ch.target = target;
        ch.remaining = cast_time.raw() > 0 ? cast_time : Fixed::from_raw(1);
        ch.damage = damage;
        ch.range = range;
        ch.hold_radius = hold_radius;
        ch.anchor_x = c.x;
        ch.anchor_y = c.y;
        ch.active = true;
        channels_.push_back(ch);
        return int(channels_.size()) - 1;
    }

    bool active(int handle) const {
        return handle >= 0 && std::size_t(handle) < channels_.size() && channels_[handle].active;
    }
    int active_count() const {
        int n = 0;
        for (const Channel& c : channels_)
            if (c.active) ++n;
        return n;
    }

    int advance(World& world, Fixed dt) {
        int landed = 0;
        for (Channel& ch : channels_) {
            if (!ch.active) continue;
            const std::uint32_t n = std::uint32_t(world.units().size());
            if (ch.caster >= n || !world.units()[ch.caster].alive) {
                ch.active = false;
                continue;
            }
            const Unit& c = world.units()[ch.caster];
            const Fixed mdx = c.x - ch.anchor_x, mdy = c.y - ch.anchor_y;
            const Fixed hr = ch.hold_radius;
            if (mdx * mdx + mdy * mdy > hr * hr) {
                ch.active = false;
                continue;
            }

            ch.remaining = ch.remaining - dt;
            if (ch.remaining.raw() > 0) continue;
            ch.active = false;

            if (ch.target >= n) continue;
            Unit& t = world.unit(ch.target);
            if (!t.alive) continue;
            if (ch.range.raw() > 0) {
                const Fixed dx = t.x - c.x, dy = t.y - c.y;
                if (dx * dx + dy * dy > ch.range * ch.range) continue;
            }
            apply(t, ch.damage);
            ++landed;
        }
        return landed;
    }

private:
    struct Channel {
        std::uint32_t caster = kNoTarget;
        std::uint32_t target = kNoTarget;
        Fixed remaining;
        Fixed damage;
        Fixed range;
        Fixed hold_radius;
        Fixed anchor_x, anchor_y;
        bool active = false;
    };

    static void apply(Unit& t, Fixed dmg) {
        if (t.shields.raw() > 0) {
            if (t.shields >= dmg) {
                t.shields = t.shields - dmg;
                return;
            }
            dmg = dmg - t.shields;
            t.shields = Fixed();
        }
        t.hp = t.hp - dmg;
        if (t.hp.raw() <= 0) t.alive = false;
    }

    std::vector<Channel> channels_;
};

}
