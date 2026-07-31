#pragma once

#include <cstdint>
#include <vector>

#include "sc2/sim/world.hpp"

namespace sc2::sim {

class ContaminateSystem {
public:
    void contaminate(std::uint32_t producer, Fixed duration) {
        if (duration.raw() <= 0) return;
        for (Hold& h : holds_)
            if (h.producer == producer) {
                h.timer = duration;
                return;
            }
        holds_.push_back(Hold{producer, duration});
    }

    bool contaminated(std::uint32_t producer) const {
        for (const Hold& h : holds_)
            if (h.producer == producer) return true;
        return false;
    }
    int active_count() const { return int(holds_.size()); }

    void advance(World& world, Fixed dt) {
        std::vector<Hold> kept;
        kept.reserve(holds_.size());
        for (Hold& h : holds_) {
            world.boost_production(h.producer, -dt);
            h.timer = h.timer - dt;
            if (h.timer.raw() > 0) kept.push_back(h);
        }
        holds_.swap(kept);
    }

private:
    struct Hold {
        std::uint32_t producer = kNoTarget;
        Fixed timer;
    };
    std::vector<Hold> holds_;
};

}
