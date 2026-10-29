#pragma once

#include <cstdint>
#include <vector>

#include "omc/sim/world.hpp"

namespace omc::sim {

class StimSystem {
public:
    void stim(World& world, std::uint32_t id, Fixed life_cost, Fixed atk_speed_mult,
              Fixed move_speed_mult, Fixed duration) {
        if (id >= world.units().size()) return;
        Unit& u = world.unit(id);
        if (!u.alive) return;

        Active* cur = find(id);
        if (!cur) {
            if (life_cost.raw() > 0) {
                const Fixed before = u.hp;
                u.hp = u.hp - life_cost;
                if (u.hp.raw() < Fixed::from_int(1).raw()) u.hp = Fixed::from_int(1);
                u.life_paid = u.life_paid + (before - u.hp);
            }
            actives_.push_back(Active{id, duration.raw() > 0 ? duration : Fixed::from_raw(1)});
        } else {
            cur->timer = duration.raw() > 0 ? duration : Fixed::from_raw(1);
        }

        u.atk_speed_mult = atk_speed_mult;
        BehaviorInstance b;
        b.kind = kStimBehaviorKind;
        b.timer = duration.raw() > 0 ? duration : Fixed::from_raw(1);
        b.speed_mult = move_speed_mult;
        world.add_behavior(id, b);
    }

    bool stimmed(std::uint32_t id) const { return const_cast<StimSystem*>(this)->find(id) != nullptr; }
    int active_count() const { return int(actives_.size()); }

    void advance(World& world, Fixed dt) {
        const std::uint32_t n = std::uint32_t(world.units().size());
        std::vector<Active> kept;
        kept.reserve(actives_.size());
        for (Active& a : actives_) {
            a.timer = a.timer - dt;
            const bool alive = a.id < n && world.units()[a.id].alive;
            if (a.timer.raw() > 0 && alive) {
                kept.push_back(a);
                continue;
            }
            if (a.id < n) {
                Unit& u = world.unit(a.id);
                u.atk_speed_mult = Fixed::from_int(1);
                for (BehaviorInstance& b : u.behaviors)
                    if (b.kind == kStimBehaviorKind) b.timer = Fixed();
            }
        }
        actives_.swap(kept);
    }

    static constexpr std::uint16_t kStimBehaviorKind = 0x5713;

private:
    struct Active {
        std::uint32_t id = kNoTarget;
        Fixed timer;
    };
    Active* find(std::uint32_t id) {
        for (Active& a : actives_)
            if (a.id == id) return &a;
        return nullptr;
    }
    std::vector<Active> actives_;
};

}
