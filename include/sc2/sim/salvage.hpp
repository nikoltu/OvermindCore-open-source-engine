#pragma once

#include <cstdint>

#include "sc2/sim/world.hpp"

namespace sc2::sim {

struct SalvageResult {
    int minerals = 0;
    int gas = 0;
    bool ok = false;
};

inline SalvageResult salvage(World& world, std::uint32_t structure, int mineral_cost,
                             int gas_cost, Fixed refund_frac = Fixed::from_ratio(3, 4)) {
    SalvageResult r;
    if (structure >= world.units().size()) return r;
    Unit& u = world.unit(structure);
    if (!u.alive || u.player < 0) return r;

    if (refund_frac.raw() < 0) refund_frac = Fixed();
    r.minerals = (Fixed::from_int(mineral_cost) * refund_frac).floor();
    r.gas = (Fixed::from_int(gas_cost) * refund_frac).floor();
    if (r.minerals < 0) r.minerals = 0;
    if (r.gas < 0) r.gas = 0;

    Player& p = world.player(u.player);
    p.minerals += r.minerals;
    p.gas += r.gas;
    u.alive = false;
    r.ok = true;
    return r;
}

}
