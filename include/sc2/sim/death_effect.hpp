#pragma once

#include <cstdint>
#include <vector>

#include "sc2/sim/world.hpp"

namespace sc2::sim {

struct DeathEffect {
    Unit spawn_proto;
    int32_t spawn_count = 0;
    Fixed spawn_spread;
    Fixed damage;
    Fixed radius;
    int32_t owner = -1;
};

class DeathEffectSystem {
public:
    void register_effect(const World& world, std::uint32_t id, DeathEffect effect) {
        if (id < world.units().size() && effect.owner < 0) effect.owner = world.units()[id].player;
        pending_.push_back(Entry{id, effect, false});
    }

    int observe(World& world) {
        int resolved = 0;
        for (Entry& e : pending_) {
            if (e.done) continue;
            if (e.id >= world.units().size()) { e.done = true; continue; }
            const Unit& dead = world.unit(e.id);
            if (dead.alive) continue;
            e.done = true;
            ++resolved;
            const Fixed dx0 = dead.x, dy0 = dead.y;

            if (e.effect.damage.raw() > 0 && e.effect.radius.raw() > 0) {
                const std::uint32_t n = std::uint32_t(world.units().size());
                const Fixed r2 = e.effect.radius * e.effect.radius;
                for (std::uint32_t i = 0; i < n; ++i) {
                    Unit& t = world.unit(i);
                    if (!t.alive) continue;
                    if (world.are_allied(t.player, e.effect.owner)) continue;
                    const Fixed ddx = t.x - dx0, ddy = t.y - dy0;
                    if (ddx * ddx + ddy * ddy > r2) continue;
                    apply_damage(t, e.effect.damage);
                }
            }

            for (int i = 0; i < e.effect.spawn_count; ++i) {
                Unit u = e.effect.spawn_proto;
                u.player = e.effect.owner;
                u.alive = true;
                Fixed ox, oy;
                ring_offset(i, e.effect.spawn_count, e.effect.spawn_spread, ox, oy);
                u.x = dx0 + ox;
                u.y = dy0 + oy;
                world.add_unit(u);
            }
        }
        return resolved;
    }

    std::size_t pending() const {
        std::size_t n = 0;
        for (const Entry& e : pending_)
            if (!e.done) ++n;
        return n;
    }

private:
    struct Entry {
        std::uint32_t id;
        DeathEffect effect;
        bool done;
    };

    static void apply_damage(Unit& t, Fixed dmg) {
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

    static void ring_offset(int i, int n, Fixed spread, Fixed& ox, Fixed& oy) {
        if (spread.raw() <= 0 || n <= 1) {
            ox = Fixed::from_raw(spread.raw() ? spread.raw() : (i * 64));
            oy = Fixed();
            return;
        }
        static const int DIR[8][2] = {{1, 0}, {1, 1}, {0, 1}, {-1, 1},
                                      {-1, 0}, {-1, -1}, {0, -1}, {1, -1}};
        const int k = i % 8;
        const bool diag = DIR[k][0] != 0 && DIR[k][1] != 0;
        Fixed s = spread;
        if (diag) s = Fixed::from_raw(spread.raw() * 181 / 256);
        ox = s * Fixed::from_int(DIR[k][0]);
        oy = s * Fixed::from_int(DIR[k][1]);
    }

    std::vector<Entry> pending_;
};

}
