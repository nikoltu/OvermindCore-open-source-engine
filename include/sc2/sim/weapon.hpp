#pragma once

#include <cstdint>

#include "sc2/sim/combat.hpp"
#include "sc2/sim/fixed.hpp"

namespace sc2::sim {

enum UnitAttribute : std::uint32_t {
    kAttrLight = 1u << 0,
    kAttrArmored = 1u << 1,
    kAttrBiological = 1u << 2,
    kAttrMechanical = 1u << 3,
    kAttrMassive = 1u << 4,
    kAttrPsionic = 1u << 5,
    kAttrStructure = 1u << 6,
};

enum CollideFlag : std::uint32_t {
    kCollideGround = 1u << 0,
    kCollideSmall = 1u << 1,
    kCollideFlying = 1u << 2,
    kCollideFlyingEscorts = 1u << 3,
    kCollideStructure = 1u << 4,
    kCollideForceField = 1u << 5,
    kCollideLocust = 1u << 6,
    kCollideLarva = 1u << 7,
    kCollideColossus = 1u << 8,
    kCollideBurrow = 1u << 9,
    kCollideRoachBurrow = 1u << 10,
    kCollidePhased = 1u << 11,
    kCollideCreepTumor = 1u << 12,
    kCollideDisruptorPhased = 1u << 13,
    kCollideLocustForceField = 1u << 14,
};

enum WeaponTarget : std::uint32_t {
    kTargetGround = 1u << 0,
    kTargetAir = 1u << 1,
};

enum class SplashShape { None, Circle, Line };

struct WeaponDef {
    Fixed damage;
    Fixed period;
    Fixed range;
    Fixed damage_point;
    Fixed backswing;
    std::uint32_t target_filter = kTargetGround | kTargetAir;
    Fixed projectile_speed;

    std::uint32_t bonus_vs = 0;
    Fixed bonus;

    SplashShape splash = SplashShape::None;
    Fixed splash_inner, splash_mid, splash_outer;

    int bounce_count = 0;
    Fixed bounce_range;
    Fixed bounce_falloff = Fixed::from_ratio(1, 3);
};

inline bool weapon_can_target(const WeaponDef& w, bool target_flying) {
    return target_flying ? (w.target_filter & kTargetAir) != 0
                         : (w.target_filter & kTargetGround) != 0;
}

inline Fixed weapon_base_damage(const WeaponDef& w, std::uint32_t attrs) {
    Fixed d = w.damage;
    if (w.bonus_vs && (attrs & w.bonus_vs)) d = d + w.bonus;
    return d;
}

inline Fixed weapon_effective_damage(const WeaponDef& w, std::uint32_t attrs, Fixed armor) {
    Fixed eff = weapon_base_damage(w, attrs) - armor;
    if (eff < min_damage()) eff = min_damage();
    return eff;
}

inline Fixed splash_factor(const WeaponDef& w, Fixed dist) {
    const Fixed one = Fixed::from_int(1);
    if (w.splash == SplashShape::None) return dist.raw() <= 0 ? one : Fixed();
    if (dist <= w.splash_inner) return one;
    if (dist <= w.splash_mid) return Fixed::from_ratio(1, 2);
    if (dist <= w.splash_outer) return Fixed::from_ratio(1, 4);
    return Fixed();
}

enum class FirePhase { Ready, Windup, Recover };

struct FireFsm {
    FirePhase phase = FirePhase::Ready;
    Fixed timer;
    Fixed recover_left;

    bool ready() const { return phase == FirePhase::Ready; }

    bool begin(const WeaponDef& w) {
        if (phase != FirePhase::Ready) return false;
        phase = FirePhase::Windup;
        timer = w.damage_point;
        recover_left = w.period - w.damage_point;
        if (recover_left.raw() < 0) recover_left = Fixed();
        return true;
    }

    bool advance(Fixed dt) {
        if (phase == FirePhase::Windup) {
            timer = timer - dt;
            if (timer.raw() <= 0) {
                phase = FirePhase::Recover;
                timer = recover_left;
                return true;
            }
            return false;
        }
        if (phase == FirePhase::Recover) {
            timer = timer - dt;
            if (timer.raw() <= 0) phase = FirePhase::Ready;
        }
        return false;
    }
};

}
