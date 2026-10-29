#pragma once

#include <cstdint>
#include <vector>

#include "omc/sim/world.hpp"

namespace omc::sim {

class SupplyDropSystem {
public:
    static constexpr double kEnRouteDelay = 4.0;
    static constexpr int kSupplyBonus = 8;
    static constexpr int kLifeBonus = 100;

    void queue(std::uint32_t depot, Fixed delay) {
        for (const Drop& d : pending_)
            if (d.depot == depot && !d.applied) return;
        pending_.push_back(Drop{depot, delay, false});
    }
    int pending_count() const {
        int n = 0;
        for (const Drop& d : pending_)
            if (!d.applied) ++n;
        return n;
    }

    void advance(World& world, Fixed dt) {
        for (Drop& d : pending_) {
            if (d.applied) continue;
            d.timer = d.timer - dt;
            if (d.timer.raw() > 0) continue;
            d.applied = true;
            if (d.depot >= world.units().size()) continue;
            Unit& u = world.unit(d.depot);
            if (!u.alive) continue;
            u.supply_provides = u.supply_provides + Fixed::from_int(kSupplyBonus);
            u.hp_max = u.hp_max + Fixed::from_int(kLifeBonus);
            u.hp = u.hp + Fixed::from_int(kLifeBonus);
        }
    }

private:
    struct Drop {
        std::uint32_t depot = kNoTarget;
        Fixed timer;
        bool applied = false;
    };
    std::vector<Drop> pending_;
};

}
