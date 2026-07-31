#pragma once

#include <cstdint>

#include "sc2/sim/fixed.hpp"

namespace sc2::sim {

struct Defender {
    Fixed life;
    Fixed shields;
    Fixed armor;
    Fixed damage_reduction = Fixed();
};

struct Weapon {
    Fixed damage;
    Fixed period;
};

inline Fixed min_damage() { return Fixed::from_ratio(1, 2); }

inline void apply_hit(const Weapon& w, Defender& d) {
    Fixed eff = w.damage - d.armor - d.damage_reduction;
    if (eff < min_damage()) eff = min_damage();
    if (d.shields.raw() > 0) {
        if (eff <= d.shields) {
            d.shields = d.shields - eff;
            return;
        }
        eff = eff - d.shields;
        d.shields = Fixed();
    }
    d.life = d.life - eff;
}

struct CombatResult {
    int hits = 0;
    Fixed duration;
};

inline CombatResult time_to_kill(const Weapon& w, Defender d) {
    CombatResult r;
    const int kSafetyCap = 100000;
    while (d.life.raw() > 0 && r.hits < kSafetyCap) {
        apply_hit(w, d);
        ++r.hits;
    }
    if (r.hits > 0) r.duration = w.period * Fixed::from_int(r.hits - 1);
    return r;
}

}
