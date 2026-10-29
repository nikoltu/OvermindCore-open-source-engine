#pragma once

#include <cstdint>

#include "omc/sim/world.hpp"

namespace omc::sim {

struct TransfusionResult {
    Fixed healed;
    bool ok = false;
};

inline TransfusionResult transfuse(World& world, std::uint32_t target, Fixed amount) {
    TransfusionResult r;
    if (target >= world.units().size() || amount.raw() <= 0) return r;
    const Unit& t = world.unit(target);
    if (!t.alive || t.hp.raw() <= 0) return r;
    r.ok = true;
    r.healed = world.heal_unit(target, amount);
    return r;
}

}
