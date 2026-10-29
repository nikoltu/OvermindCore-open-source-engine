#pragma once

#include <cstdint>
#include <vector>

#include "omc/sim/world.hpp"

namespace omc::sim {

struct Aura {
    std::uint32_t source = kNoTarget;
    Fixed radius;
    Fixed heal_per_sec;
    bool heal_self = false;

    std::uint16_t buff_kind = 0;
    Fixed buff_speed_mult = Fixed::from_int(1);
    Fixed buff_ttl = Fixed::from_ratio(1, 4);
};

class AuraSystem {
public:
    int32_t add(const Aura& a) {
        auras_.push_back(a);
        return int32_t(auras_.size()) - 1;
    }
    std::size_t count() const { return auras_.size(); }
    Aura& at(std::size_t i) { return auras_[i]; }

    void advance(World& world, Fixed dt) {
        const std::uint32_t n = std::uint32_t(world.units().size());
        for (const Aura& a : auras_) {
            if (a.source >= n) continue;
            const Unit& src = world.units()[a.source];
            if (!src.alive || a.radius.raw() <= 0) continue;
            const Fixed sx = src.x, sy = src.y;
            const int32_t sp = src.player;
            const Fixed r2 = a.radius * a.radius;
            const Fixed heal = a.heal_per_sec * dt;

            for (std::uint32_t i = 0; i < n; ++i) {
                Unit& t = world.unit(i);
                if (!t.alive) continue;
                if (!world.are_allied(t.player, sp)) continue;
                const Fixed ddx = t.x - sx, ddy = t.y - sy;
                if (ddx * ddx + ddy * ddy > r2) continue;

                if (heal.raw() > 0 && (a.heal_self || i != a.source) && t.hp.raw() > 0 &&
                    t.hp < t.hp_max) {
                    t.hp = t.hp + heal;
                    if (t.hp > t.hp_max) t.hp = t.hp_max;
                }

                if (a.buff_kind != 0) {
                    BehaviorInstance b;
                    b.kind = a.buff_kind;
                    b.timer = a.buff_ttl;
                    b.speed_mult = a.buff_speed_mult;
                    world.add_behavior(i, b);
                }
            }
        }
    }

private:
    std::vector<Aura> auras_;
};

}
