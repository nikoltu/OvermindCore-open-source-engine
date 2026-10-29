#pragma once

#include <cstdint>
#include <vector>

#include "omc/sim/world.hpp"

namespace omc::sim {

class GuardianShieldSystem {
public:
    int32_t add(std::uint32_t source, Fixed radius, Fixed amount) {
        sources_.push_back(Source{source, radius, amount});
        return int32_t(sources_.size()) - 1;
    }
    std::size_t count() const { return sources_.size(); }

    void advance(World& world) {
        const std::uint32_t n = std::uint32_t(world.units().size());

        std::vector<Fixed> cover(n, Fixed());
        std::vector<std::uint8_t> covered(n, 0);
        for (const Source& s : sources_) {
            if (s.source >= n) continue;
            const Unit& src = world.units()[s.source];
            if (!src.alive || s.radius.raw() <= 0 || s.amount.raw() <= 0) continue;
            const Fixed r2 = s.radius * s.radius;
            for (std::uint32_t i = 0; i < n; ++i) {
                const Unit& u = world.units()[i];
                if (!u.alive || !world.are_allied(u.player, src.player)) continue;
                const Fixed dx = u.x - src.x, dy = u.y - src.y;
                if (dx * dx + dy * dy > r2) continue;
                covered[i] = 1;
                if (s.amount > cover[i]) cover[i] = s.amount;
            }
        }

        if (applied_.size() < n) applied_.resize(n, 0);
        for (std::uint32_t i = 0; i < n; ++i) {
            Unit& u = world.unit(i);
            if (covered[i] && u.alive) {
                if (applied_[i]) {
                    u.damage_reduction = u.damage_reduction - applied_prev_[i] + cover[i];
                } else {
                    applied_[i] = 1;
                    u.damage_reduction = u.damage_reduction + cover[i];
                }
                remember(i, cover[i]);
            } else if (applied_[i]) {
                u.damage_reduction = u.damage_reduction - applied_prev_[i];
                if (u.damage_reduction.raw() < 0) u.damage_reduction = Fixed();
                applied_[i] = 0;
                remember(i, Fixed());
            }
        }
    }

private:
    struct Source {
        std::uint32_t source = kNoTarget;
        Fixed radius;
        Fixed amount;
    };
    void remember(std::uint32_t i, Fixed v) {
        if (applied_prev_.size() <= i) applied_prev_.resize(i + 1, Fixed());
        applied_prev_[i] = v;
    }

    std::vector<Source> sources_;
    std::vector<std::uint8_t> applied_;
    std::vector<Fixed> applied_prev_;
};

}
