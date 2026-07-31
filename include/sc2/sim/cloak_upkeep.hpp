#pragma once

#include <cstdint>
#include <vector>

#include "sc2/sim/world.hpp"

namespace sc2::sim {

class CloakUpkeepSystem {
public:
    int add(std::uint32_t unit, Fixed drain_per_sec) {
        cloakers_.push_back(Cloaker{unit, drain_per_sec, false});
        return int(cloakers_.size()) - 1;
    }

    void set(World& world, int handle, bool engage) {
        if (handle < 0 || std::size_t(handle) >= cloakers_.size()) return;
        Cloaker& c = cloakers_[handle];
        if (c.unit >= world.units().size()) return;
        Unit& u = world.unit(c.unit);
        if (engage) {
            if (u.energy.raw() <= 0) return;
            c.engaged = true;
            u.cloaked = true;
        } else {
            c.engaged = false;
            u.cloaked = false;
        }
    }

    bool engaged(int handle) const {
        return handle >= 0 && std::size_t(handle) < cloakers_.size() && cloakers_[handle].engaged;
    }

    void advance(World& world, Fixed dt) {
        for (Cloaker& c : cloakers_) {
            if (!c.engaged) continue;
            if (c.unit >= world.units().size() || !world.units()[c.unit].alive) {
                c.engaged = false;
                continue;
            }
            Unit& u = world.unit(c.unit);
            u.energy = u.energy - c.drain * dt;
            if (u.energy.raw() <= 0) {
                u.energy = Fixed();
                c.engaged = false;
                u.cloaked = false;
            } else {
                u.cloaked = true;
            }
        }
    }

private:
    struct Cloaker {
        std::uint32_t unit = kNoTarget;
        Fixed drain;
        bool engaged = false;
    };
    std::vector<Cloaker> cloakers_;
};

}
