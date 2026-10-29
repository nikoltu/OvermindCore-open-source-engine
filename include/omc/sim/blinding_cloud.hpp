#pragma once

#include <cstdint>
#include <unordered_map>
#include <vector>

#include "omc/sim/world.hpp"

namespace omc::sim {

class BlindingCloudSystem {
public:
    int add(std::int32_t owner, Fixed x, Fixed y, Fixed radius, Fixed duration) {
        clouds_.push_back(Cloud{owner, x, y, radius, duration.raw() > 0 ? duration : Fixed::from_raw(1)});
        return int(clouds_.size()) - 1;
    }
    int active_count() const {
        int n = 0;
        for (const Cloud& c : clouds_)
            if (c.active) ++n;
        return n;
    }
    bool suppressed(std::uint32_t id) const { return saved_.find(id) != saved_.end(); }

    void advance(World& world, Fixed dt) {
        const std::uint32_t n = std::uint32_t(world.units().size());

        std::vector<std::uint8_t> covered(n, 0);
        for (Cloud& c : clouds_) {
            if (!c.active) continue;
            const Fixed r2 = c.radius * c.radius;
            for (std::uint32_t i = 0; i < n; ++i) {
                const Unit& u = world.units()[i];
                if (!u.alive || world.are_allied(u.player, c.owner)) continue;
                const Fixed dx = u.x - c.x, dy = u.y - c.y;
                if (dx * dx + dy * dy <= r2) covered[i] = 1;
            }
        }

        for (std::uint32_t i = 0; i < n; ++i) {
            Unit& u = world.unit(i);
            auto it = saved_.find(i);
            if (covered[i] && u.alive) {
                if (it == saved_.end()) {
                    saved_[i] = u.wpn_range;
                    u.wpn_range = Fixed();
                }
            } else if (it != saved_.end()) {
                if (u.alive) u.wpn_range = it->second;
                saved_.erase(it);
            }
        }

        for (Cloud& c : clouds_) {
            if (!c.active) continue;
            c.timer = c.timer - dt;
            if (c.timer.raw() <= 0) c.active = false;
        }
    }

private:
    struct Cloud {
        std::int32_t owner = -1;
        Fixed x, y, radius, timer;
        bool active = true;
    };
    std::vector<Cloud> clouds_;
    std::unordered_map<std::uint32_t, Fixed> saved_;
};

}
