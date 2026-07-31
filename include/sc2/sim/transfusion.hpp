#pragma once

#include <cstdint>

#include "sc2/sim/world.hpp"

namespace sc2::sim {

struct TransfusionResult {
    Fixed healed;
    bool ok = false;
};

inline TransfusionResult transfuse(World& world, std::uint32_t target, Fixed amount) {
    TransfusionResult r;
    if (target >= world.units().size() || amount.raw() <= 0) return r;
    Unit& t = world.unit(target);
    if (!t.alive || t.hp.raw() <= 0) return r;
    if (t.hp >= t.hp_max) {
        r.ok = true;
        return r;
    }
    Fixed heal = amount;
    const Fixed missing = t.hp_max - t.hp;
    if (heal > missing) heal = missing;
    t.hp = t.hp + heal;
    r.healed = heal;
    r.ok = true;
    return r;
}

}
