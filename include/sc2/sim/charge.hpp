#pragma once

#include <cstdint>
#include <unordered_set>

#include "sc2/sim/world.hpp"

namespace sc2::sim {

class ChargeSystem {
public:
    static constexpr std::uint16_t kChargeBehaviorKind = 0xC0DE;

    void register_charger(std::uint32_t id) { chargers_.insert(id); }
    void unregister(std::uint32_t id) { chargers_.erase(id); }
    bool is_charging(std::uint32_t id) const { return charging_.count(id) != 0; }
    int charging_count() const { return int(charging_.size()); }

    void advance(World& world, Fixed dt, Fixed charge_range, Fixed melee_range, Fixed speed_mult,
                 Fixed buff_ttl = Fixed::from_ratio(1, 4)) {
        (void)dt;
        charging_.clear();
        const std::uint32_t n = std::uint32_t(world.units().size());
        const Fixed cr2 = charge_range * charge_range;
        const Fixed mr2 = melee_range * melee_range;
        for (std::uint32_t id : chargers_) {
            if (id >= n) continue;
            Unit& u = world.unit(id);
            if (!u.alive || u.orders.empty()) continue;
            const Order& o = u.orders.front();
            if (o.type != OrderType::Attack || o.target >= n) continue;
            const Unit& t = world.units()[o.target];
            if (!t.alive) continue;
            const Fixed dx = t.x - u.x, dy = t.y - u.y;
            const Fixed d2 = dx * dx + dy * dy;
            if (d2.raw() <= mr2.raw() || d2.raw() > cr2.raw()) continue;

            BehaviorInstance b;
            b.kind = kChargeBehaviorKind;
            b.timer = buff_ttl;
            b.speed_mult = speed_mult;
            world.add_behavior(id, b);
            charging_.insert(id);
        }
    }

private:
    std::unordered_set<std::uint32_t> chargers_;
    std::unordered_set<std::uint32_t> charging_;
};

}
