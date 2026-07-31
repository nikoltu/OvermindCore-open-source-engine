#pragma once

#include <cstdint>
#include <vector>

#include "sc2/sim/world.hpp"

namespace sc2::sim {

struct Blip {
    Fixed x, y;
    std::uint32_t unit = kNoTarget;
};

class SensorTowerSystem {
public:
    int add(std::int32_t owner, Fixed x, Fixed y, Fixed radius) {
        towers_.push_back(Tower{owner, x, y, radius});
        return int(towers_.size()) - 1;
    }
    std::size_t count() const { return towers_.size(); }

    std::vector<Blip> blips(const World& world, std::int32_t viewer) const {
        std::vector<Blip> out;
        const std::uint32_t n = std::uint32_t(world.units().size());
        std::vector<std::uint8_t> seen(n, 0);
        for (const Tower& t : towers_) {
            if (t.owner != viewer) continue;
            const Fixed r2 = t.radius * t.radius;
            for (std::uint32_t i = 0; i < n; ++i) {
                if (seen[i]) continue;
                const Unit& u = world.units()[i];
                if (!u.alive || u.loaded || world.are_allied(u.player, viewer)) continue;
                const Fixed dx = u.x - t.x, dy = u.y - t.y;
                if ((dx * dx + dy * dy).raw() > r2.raw()) continue;
                seen[i] = 1;
                out.push_back(Blip{u.x, u.y, i});
            }
        }
        return out;
    }

private:
    struct Tower {
        std::int32_t owner = -1;
        Fixed x, y, radius;
    };
    std::vector<Tower> towers_;
};

}
