#pragma once

#include <cstdint>
#include <vector>

#include "sc2/sim/world.hpp"

namespace sc2::sim {

class CloakFieldSystem {
public:
    int32_t add(std::uint32_t source, Fixed radius) {
        sources_.push_back(Source{source, radius});
        return int32_t(sources_.size()) - 1;
    }
    std::size_t count() const { return sources_.size(); }

    void advance(World& world) {
        const std::uint32_t n = std::uint32_t(world.units().size());

        std::vector<std::uint8_t> covered(n, 0);
        for (const Source& s : sources_) {
            if (s.source >= n) continue;
            const Unit& src = world.units()[s.source];
            if (!src.alive || s.radius.raw() <= 0) continue;
            const Fixed r2 = s.radius * s.radius;
            for (std::uint32_t i = 0; i < n; ++i) {
                const Unit& u = world.units()[i];
                if (!u.alive || !world.are_allied(u.player, src.player)) continue;
                const Fixed dx = u.x - src.x, dy = u.y - src.y;
                if (dx * dx + dy * dy <= r2) covered[i] = 1;
            }
        }

        if (field_cloaked_.size() < n) field_cloaked_.resize(n, 0);

        for (std::uint32_t i = 0; i < n; ++i) {
            Unit& u = world.unit(i);
            if (covered[i] && u.alive) {
                if (!u.cloaked) {
                    u.cloaked = true;
                    field_cloaked_[i] = 1;
                }
            } else if (field_cloaked_[i]) {
                if (u.cloaked) u.cloaked = false;
                field_cloaked_[i] = 0;
            }
        }
    }

private:
    struct Source {
        std::uint32_t source = kNoTarget;
        Fixed radius;
    };
    std::vector<Source> sources_;
    std::vector<std::uint8_t> field_cloaked_;
};

}
