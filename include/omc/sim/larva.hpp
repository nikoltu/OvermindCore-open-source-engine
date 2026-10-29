#pragma once

#include <cstdint>
#include <unordered_map>
#include <vector>

#include "omc/sim/world.hpp"

namespace omc::sim {

class LarvaSystem {
public:
    Fixed larva_period = Fixed::from_int(11);
    int32_t base_cap = 3;
    int32_t inject_cap = 19;

    void register_hatchery(std::uint32_t id) {
        Hatch h;
        h.larva = base_cap;
        h.timer = larva_period;
        hatch_[id] = h;
    }
    int32_t larva(std::uint32_t id) const {
        auto it = hatch_.find(id);
        return it == hatch_.end() ? 0 : it->second.larva;
    }

    void inject(std::uint32_t id, int32_t count) {
        auto it = hatch_.find(id);
        if (it == hatch_.end()) return;
        it->second.larva += count;
        if (it->second.larva > inject_cap) it->second.larva = inject_cap;
    }

    void inject(std::uint32_t id, int32_t count, Fixed delay) {
        if (delay.raw() <= 0) { inject(id, count); return; }
        if (hatch_.find(id) == hatch_.end()) return;
        injects_.push_back(Inject{id, count, delay});
    }

    std::uint32_t begin_morph(World& world, std::uint32_t hatchery, const Unit& finished,
                              Fixed ex, Fixed ey, Fixed build_time) {
        auto it = hatch_.find(hatchery);
        if (it == hatch_.end() || it->second.larva <= 0) return kNoTarget;
        --it->second.larva;

        Unit egg;
        egg.player = finished.player;
        egg.x = ex;
        egg.y = ey;
        egg.hp = Fixed::from_int(1);
        egg.hp_max = Fixed::from_int(1);
        egg.alive = true;
        egg.speed = Fixed();
        const std::uint32_t id = world.add_unit(egg);

        Pending p;
        p.egg = id;
        p.finished = finished;
        p.timer = build_time.raw() > 0 ? build_time : Fixed::from_int(1);
        pending_.push_back(p);
        return id;
    }

    bool is_morphing(std::uint32_t egg) const {
        for (const Pending& p : pending_)
            if (p.egg == egg) return true;
        return false;
    }

    void advance(World& world, Fixed dt) {
        for (auto& kv : hatch_) {
            Hatch& h = kv.second;
            if (h.larva >= base_cap) continue;
            h.timer = h.timer - dt;
            if (h.timer.raw() <= 0) {
                ++h.larva;
                h.timer = larva_period;
            }
        }
        for (std::size_t i = 0; i < pending_.size();) {
            Pending& p = pending_[i];
            p.timer = p.timer - dt;
            if (p.timer.raw() <= 0) {
                hatch_egg(world, p);
                pending_.erase(pending_.begin() + std::ptrdiff_t(i));
            } else {
                ++i;
            }
        }
        for (std::size_t i = 0; i < injects_.size();) {
            Inject& in = injects_[i];
            in.timer = in.timer - dt;
            if (in.timer.raw() <= 0) {
                inject(in.hatch, in.count);
                injects_.erase(injects_.begin() + std::ptrdiff_t(i));
            } else {
                ++i;
            }
        }
    }

private:
    struct Hatch {
        int32_t larva = 0;
        Fixed timer;
    };
    struct Pending {
        std::uint32_t egg = kNoTarget;
        Unit finished;
        Fixed timer;
    };
    struct Inject {
        std::uint32_t hatch = kNoTarget;
        int32_t count = 0;
        Fixed timer;
    };

    static void hatch_egg(World& world, const Pending& p) {
        if (p.egg >= world.units().size()) return;
        Unit& u = world.unit(p.egg);
        if (!u.alive) return;
        const Fixed x = u.x, y = u.y;
        u = p.finished;
        u.id = p.egg;
        u.x = x;
        u.y = y;
        u.alive = true;
    }

    std::unordered_map<std::uint32_t, Hatch> hatch_;
    std::vector<Pending> pending_;
    std::vector<Inject> injects_;
};

}
