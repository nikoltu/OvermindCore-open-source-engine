#pragma once

#include <cstdint>
#include <vector>

#include "omc/sim/fixed.hpp"

namespace omc::sim {

inline constexpr int kDeathRevealDurationSec = 4;
inline constexpr int kDeathRevealDefaultRadius = 2;

class DeathReveal {
public:
    int drop(std::int32_t player, Fixed x, Fixed y, Fixed radius, Fixed duration) {
        reveals_.push_back(Reveal{x, y, radius, duration.raw() > 0 ? duration : Fixed::from_raw(1), player});
        return int(reveals_.size()) - 1;
    }

    void advance(Fixed dt) {
        std::vector<Reveal> kept;
        kept.reserve(reveals_.size());
        for (Reveal& r : reveals_) {
            r.remaining = r.remaining - dt;
            if (r.remaining.raw() > 0) kept.push_back(r);
        }
        reveals_.swap(kept);
    }

    int active_count() const { return int(reveals_.size()); }

    bool covers(std::int32_t player, Fixed x, Fixed y) const {
        for (const Reveal& r : reveals_) {
            if (r.player != player) continue;
            const Fixed dx = x - r.x, dy = y - r.y;
            if (dx * dx + dy * dy <= r.radius * r.radius) return true;
        }
        return false;
    }

private:
    struct Reveal {
        Fixed x, y, radius, remaining;
        std::int32_t player;
    };
    std::vector<Reveal> reveals_;
};

}
