#pragma once

#include <cstdint>
#include <vector>

#include "sc2/sim/world.hpp"

namespace sc2::sim {

class InterceptorSystem {
public:
    void register_carrier(std::uint32_t carrier, int num, Fixed leash, const Unit& proto) {
        Carrier c;
        c.carrier = carrier;
        c.num = num < 0 ? 0 : num;
        c.leash = leash;
        c.proto = proto;
        carriers_.push_back(c);
    }
    int carrier_count() const { return int(carriers_.size()); }

    int deployed(std::uint32_t carrier, const World& world) const {
        for (const Carrier& c : carriers_)
            if (c.carrier == carrier) {
                int n = 0;
                for (std::uint32_t id : c.wing)
                    if (id < world.units().size() && world.units()[id].alive &&
                        !world.units()[id].loaded)
                        ++n;
                return n;
            }
        return 0;
    }

    void advance(World& world) {
        for (Carrier& c : carriers_) {
            if (c.carrier >= world.units().size()) continue;
            const bool carrier_alive = world.units()[c.carrier].alive;

            if (!carrier_alive) {
                for (std::uint32_t id : c.wing)
                    if (id < world.units().size()) world.unit(id).alive = false;
                continue;
            }

            const Unit host = world.units()[c.carrier];

            std::uint32_t target = kNoTarget;
            if (!host.orders.empty() && host.orders.front().type == OrderType::Attack) {
                const std::uint32_t tgt = host.orders.front().target;
                if (tgt < world.units().size() && world.units()[tgt].alive) {
                    const Unit& t = world.units()[tgt];
                    const Fixed dx = t.x - host.x, dy = t.y - host.y;
                    if (c.leash.raw() <= 0 || dx * dx + dy * dy <= c.leash * c.leash) target = tgt;
                }
            }

            if (target != kNoTarget) {
                ensure_wing(world, c, host);
                for (std::uint32_t id : c.wing) {
                    if (id >= world.units().size()) continue;
                    Unit& ic = world.unit(id);
                    if (!ic.alive) continue;
                    if (ic.loaded) {
                        ic.loaded = false;
                        ic.x = host.x;
                        ic.y = host.y;
                    }
                    ic.orders.clear();
                    ic.orders.push_back(Order{OrderType::Attack, Fixed(), Fixed(), target});
                    ic.ever_ordered = true;
                }
            } else {
                for (std::uint32_t id : c.wing) {
                    if (id >= world.units().size()) continue;
                    Unit& ic = world.unit(id);
                    if (!ic.alive) continue;
                    ic.loaded = true;
                    ic.orders.clear();
                    ic.target = kNoTarget;
                }
            }
        }
    }

private:
    struct Carrier {
        std::uint32_t carrier = kNoTarget;
        int num = 0;
        Fixed leash;
        Unit proto;
        std::vector<std::uint32_t> wing;
    };

    void ensure_wing(World& world, Carrier& c, const Unit& host) {
        while (int(c.wing.size()) < c.num) {
            Unit ic = c.proto;
            ic.player = host.player;
            ic.x = host.x;
            ic.y = host.y;
            ic.loaded = true;
            ic.alive = true;
            ic.orders.clear();
            ic.target = kNoTarget;
            c.wing.push_back(world.add_unit(ic));
        }
    }

    std::vector<Carrier> carriers_;
};

}
