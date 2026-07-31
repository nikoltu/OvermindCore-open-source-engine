#pragma once

#include <cstdint>
#include <vector>

#include "sc2/app/control_groups.hpp"
#include "sc2/sim/world.hpp"

namespace sc2::app {

struct CameraPose {
    float x = 0, y = 0, zoom = 1;
};

class CameraBookmarks {
public:
    static constexpr int kSlots = 8;

    void save(int slot, const CameraPose& p) {
        if (ok(slot)) {
            slots_[slot] = p;
            set_[slot] = true;
        }
    }
    bool recall(int slot, CameraPose& out) const {
        if (ok(slot) && set_[slot]) {
            out = slots_[slot];
            return true;
        }
        return false;
    }
    bool is_set(int slot) const { return ok(slot) && set_[slot]; }

    void set_follow(float x, float y) {
        follow_ = {x, y, follow_.zoom};
        following_ = true;
    }
    void clear_follow() { following_ = false; }
    bool follow(CameraPose& out) const {
        if (following_) {
            out = follow_;
            return true;
        }
        return false;
    }

private:
    static bool ok(int s) { return s >= 0 && s < kSlots; }
    CameraPose slots_[kSlots];
    bool set_[kSlots] = {};
    CameraPose follow_;
    bool following_ = false;
};

inline bool control_group_center(const ControlGroups& groups, int slot,
                                 const sim::World& w, float& cx, float& cy) {
    std::vector<uint32_t> ids = groups.recall(slot);
    const std::vector<sim::Unit>& units = w.units();
    double sx = 0, sy = 0;
    int n = 0;
    for (uint32_t id : ids) {
        if (id < units.size() && units[id].alive) {
            sx += units[id].x.to_double();
            sy += units[id].y.to_double();
            ++n;
        }
    }
    if (n == 0) return false;
    cx = float(sx / n);
    cy = float(sy / n);
    return true;
}

}
