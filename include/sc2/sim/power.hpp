#pragma once

#include <cstdint>
#include <vector>

#include "sc2/sim/fixed.hpp"

namespace sc2::sim {

class PowerSystem {
public:
    struct Source {
        Fixed x, y;
        Fixed radius;
        int level = 1;
    };

    void add_source(Fixed x, Fixed y, Fixed radius, int level = 1) {
        sources_.push_back(Source{x, y, radius, level});
    }

    bool is_powered(Fixed x, Fixed y) const { return power_level(x, y) > 0; }

    int power_level(Fixed x, Fixed y) const {
        int lvl = 0;
        for (const Source& s : sources_) {
            const Fixed dx = x - s.x, dy = y - s.y;
            if (dx * dx + dy * dy <= s.radius * s.radius && s.level > lvl) lvl = s.level;
        }
        return lvl;
    }

    const std::vector<Source>& sources() const { return sources_; }
    void clear() { sources_.clear(); }

private:
    std::vector<Source> sources_;
};

}
