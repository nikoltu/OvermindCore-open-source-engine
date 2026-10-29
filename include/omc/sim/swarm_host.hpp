#pragma once

#include <cstdint>
#include <vector>

#include "omc/sim/world.hpp"

namespace omc::sim {

class SwarmHostSystem {
public:
    void register_host(std::uint32_t host, int count, Fixed period, Fixed lifetime,
                       const Unit& locust) {
        Host h;
        h.host = host;
        h.count = count < 1 ? 1 : count;
        h.period = period.raw() > 0 ? period : Fixed::from_raw(1);
        h.lifetime = lifetime.raw() > 0 ? lifetime : Fixed::from_raw(1);
        h.timer = h.period;
        h.locust = locust;
        hosts_.push_back(h);
    }
    int host_count() const { return int(hosts_.size()); }

    int advance(World& world, Fixed dt) {
        int spawned = 0;
        static const int DX[6] = {2, 2, 1, -1, -2, -2};
        static const int DY[6] = {0, 1, 2, 2, 1, 0};
        for (Host& h : hosts_) {
            if (h.host >= world.units().size() || !world.units()[h.host].alive) continue;
            h.timer = h.timer - dt;
            if (h.timer.raw() > 0) continue;
            h.timer = h.timer + h.period;

            const Unit host = world.units()[h.host];
            for (int i = 0; i < h.count; ++i) {
                Unit u = h.locust;
                u.player = host.player;
                const int k = i % 6;
                u.x = host.x + Fixed::from_int(DX[k]);
                u.y = host.y + Fixed::from_int(DY[k]);
                u.time_life = h.lifetime;
                u.alive = true;
                u.orders.clear();
                u.target = kNoTarget;
                world.add_unit(u);
                ++spawned;
            }
        }
        return spawned;
    }

private:
    struct Host {
        std::uint32_t host = kNoTarget;
        int count = 1;
        Fixed period, lifetime, timer;
        Unit locust;
    };
    std::vector<Host> hosts_;
};

}
