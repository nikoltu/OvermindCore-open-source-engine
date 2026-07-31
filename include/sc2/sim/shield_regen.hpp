#pragma once

#include <cstdint>
#include <vector>

#include "sc2/sim/world.hpp"

namespace sc2::sim {

class ShieldRegenSystem {
public:
    explicit ShieldRegenSystem(Fixed regen_per_sec = Fixed::from_int(2),
                               Fixed delay = Fixed::from_int(10))
        : regen_(regen_per_sec), delay_(delay) {}

    void advance(World& world, Fixed dt) {
        const std::uint32_t n = std::uint32_t(world.units().size());
        if (state_.size() < n) state_.resize(n);

        for (std::uint32_t i = 0; i < n; ++i) {
            Unit& u = world.unit(i);
            State& s = state_[i];

            const Fixed total = u.hp + u.shields;
            if (!s.seen) {
                s.seen = true;
                s.last_total = total;
                s.since_hit = Fixed();
            }

            if (total < s.last_total) s.since_hit = Fixed();
            s.last_total = total;

            if (!u.alive || u.shields_max.raw() <= 0) continue;

            if (s.since_hit < delay_) {
                s.since_hit = s.since_hit + dt;
            } else if (u.shields < u.shields_max) {
                u.shields = u.shields + regen_ * dt;
                if (u.shields > u.shields_max) u.shields = u.shields_max;
                s.last_total = u.hp + u.shields;
            }
        }
    }

    Fixed delay_left(std::uint32_t id) const {
        if (id >= state_.size() || !state_[id].seen) return delay_;
        Fixed d = delay_ - state_[id].since_hit;
        return d.raw() > 0 ? d : Fixed();
    }

private:
    struct State {
        bool seen = false;
        Fixed last_total;
        Fixed since_hit;
    };

    Fixed regen_, delay_;
    std::vector<State> state_;
};

}
