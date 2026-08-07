#pragma once

#include <cstdint>

#include "sc2/sim/fixed.hpp"

namespace sc2::sim {

struct Defender {
    Fixed life;
    Fixed shields;
    Fixed armor;
    Fixed damage_reduction = Fixed();
    Fixed dmg_fraction = Fixed::from_int(1);
    Fixed dmg_clamp_max = Fixed();
    Fixed dmg_minimum = Fixed();
    Fixed shield_armor = Fixed();
};

struct Weapon {
    Fixed damage;
    Fixed period;
};

inline Fixed min_damage() { return Fixed::from_ratio(1, 2); }

inline void apply_hit(const Weapon& w, Defender& d) {
    const Fixed armor = d.shields.raw() > 0 ? d.shield_armor : d.armor;
    Fixed eff = w.damage - armor - d.damage_reduction;
    eff = eff * d.dmg_fraction;
    if (d.dmg_clamp_max.raw() > 0 && eff > d.dmg_clamp_max) eff = d.dmg_clamp_max;
    if (eff < d.dmg_minimum) eff = d.dmg_minimum;
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
