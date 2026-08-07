#pragma once

#include <cstdint>

#include "sc2/sim/world.hpp"

namespace sc2::sim {

struct FeedbackResult {
    Fixed damage_dealt;
    bool killed = false;
    bool had_energy = false;
};

inline FeedbackResult feedback(World& world, std::uint32_t target,
                               Fixed energy_fraction = Fixed::from_ratio(1, 2)) {
    FeedbackResult r;
    if (target >= world.units().size()) return r;
    Unit& t = world.unit(target);
    if (!t.alive || t.energy.raw() <= 0) return r;

    r.had_energy = true;
    Fixed dmg = t.energy * energy_fraction;
    r.damage_dealt = dmg;
    t.energy = Fixed();

    if (t.shields.raw() > 0) {
        if (t.shields >= dmg) {
            t.shields = t.shields - dmg;
            return r;
        }
        dmg = dmg - t.shields;
        t.shields = Fixed();
    }
    t.hp = t.hp - dmg;
    if (t.hp.raw() <= 0) {
        t.alive = false;
        r.killed = true;
    }
    return r;
}

}
