#pragma once

#include <cstdint>
#include <vector>

#include "omc/sim/fixed.hpp"

namespace omc::sim {

class ScanSweep {
public:
    int drop(Fixed x, Fixed y, Fixed radius, Fixed duration) {
        scans_.push_back(Scan{x, y, radius, duration.raw() > 0 ? duration : Fixed::from_raw(1)});
        return int(scans_.size()) - 1;
    }

    void advance(Fixed dt) {
        std::vector<Scan> kept;
        kept.reserve(scans_.size());
        for (Scan& s : scans_) {
            s.remaining = s.remaining - dt;
            if (s.remaining.raw() > 0) kept.push_back(s);
        }
        scans_.swap(kept);
    }

    int active_count() const { return int(scans_.size()); }

    bool covers(Fixed x, Fixed y) const {
        for (const Scan& s : scans_) {
            const Fixed dx = x - s.x, dy = y - s.y;
            if (dx * dx + dy * dy <= s.radius * s.radius) return true;
        }
        return false;
    }

    Fixed remaining_at(Fixed x, Fixed y) const {
        Fixed best;
        for (const Scan& s : scans_) {
            const Fixed dx = x - s.x, dy = y - s.y;
            if (dx * dx + dy * dy <= s.radius * s.radius && s.remaining > best) best = s.remaining;
        }
        return best;
    }

private:
    struct Scan {
        Fixed x, y, radius, remaining;
    };
    std::vector<Scan> scans_;
};

}
