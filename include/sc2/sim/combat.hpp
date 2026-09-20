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
    uint32_t attributes = 0;
    Fixed barrier = Fixed();
};

struct Weapon {
    Fixed damage;
    Fixed period;
    uint32_t bonus_attr = 0;
    Fixed bonus_amount = Fixed();
    Fixed armor_reduction = Fixed::from_int(1);
};

inline Fixed min_damage() { return Fixed::from_ratio(1, 2); }

inline void apply_hit(const Weapon& w, Defender& d) {
    const Fixed armor = d.shields.raw() > 0 ? d.shield_armor : d.armor;
    Fixed base = w.damage;
    if (w.bonus_attr != 0 && (d.attributes & w.bonus_attr) != 0) base = base + w.bonus_amount;
    Fixed eff = base - armor * w.armor_reduction - d.damage_reduction;
    eff = eff * d.dmg_fraction;
    if (d.dmg_clamp_max.raw() > 0 && eff > d.dmg_clamp_max) eff = d.dmg_clamp_max;
    if (eff < d.dmg_minimum) eff = d.dmg_minimum;
    if (eff < min_damage()) eff = min_damage();
    if (d.barrier.raw() > 0) {
        if (eff <= d.barrier) {
            d.barrier = d.barrier - eff;
            return;
        }
        eff = eff - d.barrier;
        d.barrier = Fixed();
    }
    if (d.shields.raw() > 0) {
        if (eff <= d.shields) {
            d.shields = d.shields - eff;
            return;
        }
        eff = eff - d.shields;
        d.shields = Fixed();
        eff = eff - d.armor * w.armor_reduction;
        if (eff.raw() <= 0) return;
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

inline bool in_line_splash(Fixed ax, Fixed ay, Fixed tx, Fixed ty, Fixed first, Fixed step, int count,
                           Fixed radius, Fixed px, Fixed py, bool from_source = false) {
    if (count <= 0 || radius.raw() <= 0) return false;
    const Fixed r2 = radius * radius;
    Fixed dirx = Fixed(), diry = Fixed();
    const Fixed bvx = tx - ax, bvy = ty - ay;
    const Fixed blen = isqrt(bvx * bvx + bvy * bvy);
    if (blen.raw() > 0) {
        dirx = bvx / blen;
        diry = bvy / blen;
    }
    const Fixed anchx = from_source ? ax : tx, anchy = from_source ? ay : ty;
    for (int k = 0; k < count; ++k) {
        const Fixed off = first + step * Fixed::from_int(k);
        const Fixed cx = anchx + dirx * off, cy = anchy + diry * off;
        const Fixed ex = px - cx, ey = py - cy;
        if (ex * ex + ey * ey <= r2) return true;
    }
    return false;
}

inline bool in_arc(Fixed ax, Fixed ay, Fixed tx, Fixed ty, Fixed radius, Fixed cos_half, Fixed px, Fixed py) {
    if (radius.raw() <= 0) return false;
    const Fixed ex = px - tx, ey = py - ty;
    const Fixed d2 = ex * ex + ey * ey;
    if (d2 > radius * radius) return false;
    const Fixed bvx = tx - ax, bvy = ty - ay;
    const Fixed blen = isqrt(bvx * bvx + bvy * bvy);
    const Fixed elen = isqrt(d2);
    if (blen.raw() <= 0 || elen.raw() <= 0) return true;
    const Fixed denom = blen * elen;
    if (denom.raw() <= 0) return (bvx * ex + bvy * ey).raw() >= 0;
    const Fixed dot = (bvx * ex + bvy * ey) / denom;
    return dot >= cos_half;
}

}
