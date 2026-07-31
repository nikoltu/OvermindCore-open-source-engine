#pragma once

#include <cstdint>
#include <vector>

#include "sc2/sim/world.hpp"

namespace sc2::sim {

class ChronoBoostSystem {
public:
    void boost(std::uint32_t producer, Fixed factor, Fixed duration) {
        if (factor.raw() <= Fixed::from_int(1).raw() || duration.raw() <= 0) return;
        for (Boost& b : boosts_)
            if (b.producer == producer) {
                b.factor = factor;
                b.timer = duration;
                return;
            }
        boosts_.push_back(Boost{producer, factor, duration});
    }

    bool boosting(std::uint32_t producer) const {
        for (const Boost& b : boosts_)
            if (b.producer == producer) return true;
        return false;
    }
    int active_count() const { return int(boosts_.size()); }

    void advance(World& world, Fixed dt) {
        std::vector<Boost> kept;
        kept.reserve(boosts_.size());
        for (Boost& b : boosts_) {
            const Fixed extra = dt * (b.factor - Fixed::from_int(1));
            world.boost_production(b.producer, extra);
            b.timer = b.timer - dt;
            if (b.timer.raw() > 0) kept.push_back(b);
        }
        boosts_.swap(kept);
    }

private:
    struct Boost {
        std::uint32_t producer = kNoTarget;
        Fixed factor;
        Fixed timer;
    };
    std::vector<Boost> boosts_;
};

}
