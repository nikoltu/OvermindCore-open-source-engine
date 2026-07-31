#pragma once

#include <cstdint>
#include <vector>

#include "sc2/sim/world.hpp"

namespace sc2::sim {

class MindControlSystem {
public:
    bool take(World& world, std::uint32_t target, std::int32_t new_owner, Fixed duration = Fixed()) {
        if (target >= world.units().size()) return false;
        Unit& u = world.unit(target);
        if (!u.alive) return false;

        Hold* h = find(target);
        if (!h) {
            holds_.push_back(Hold{target, u.player, duration, duration.raw() <= 0});
        } else {
            h->timer = duration;
            h->permanent = duration.raw() <= 0;
        }
        u.player = new_owner;
        return true;
    }

    bool controlled(std::uint32_t target) const {
        return const_cast<MindControlSystem*>(this)->find(target) != nullptr;
    }
    int active_count() const { return int(holds_.size()); }

    void advance(World& world, Fixed dt) {
        const std::uint32_t n = std::uint32_t(world.units().size());
        std::vector<Hold> kept;
        kept.reserve(holds_.size());
        for (Hold& h : holds_) {
            const bool alive = h.target < n && world.units()[h.target].alive;
            if (!alive) continue;
            if (h.permanent) {
                kept.push_back(h);
                continue;
            }
            h.timer = h.timer - dt;
            if (h.timer.raw() > 0) {
                kept.push_back(h);
            } else {
                world.unit(h.target).player = h.original_owner;
            }
        }
        holds_.swap(kept);
    }

private:
    struct Hold {
        std::uint32_t target = kNoTarget;
        std::int32_t original_owner = -1;
        Fixed timer;
        bool permanent = false;
    };
    Hold* find(std::uint32_t target) {
        for (Hold& h : holds_)
            if (h.target == target) return &h;
        return nullptr;
    }
    std::vector<Hold> holds_;
};

}
