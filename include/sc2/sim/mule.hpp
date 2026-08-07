#pragma once

#include <cstdint>
#include <vector>

#include "sc2/sim/world.hpp"

namespace sc2::sim {

class MuleSystem {
public:
    struct MuleStats {
        int32_t carry_amount = 25;
        Fixed hp = Fixed::from_int(60);
        Fixed speed = Fixed::from_ratio(63, 16);
        Fixed mine_time = Fixed::from_ratio(1, 2);
        Fixed radius = Fixed::from_ratio(3, 8);
    };
    static constexpr int kLifetimeSeconds = 90;

    std::uint32_t calldown(World& world, int32_t player, Fixed x, Fixed y, std::uint32_t node,
                           std::uint32_t depot, Fixed lifetime) {
        return calldown(world, player, x, y, node, depot, lifetime, MuleStats());
    }
    std::uint32_t calldown(World& world, int32_t player, Fixed x, Fixed y, std::uint32_t node,
                           std::uint32_t depot, Fixed lifetime, const MuleStats& st) {
        Unit m;
        m.player = player;
        m.x = x;
        m.y = y;
        m.hp = st.hp;
        m.hp_max = st.hp;
        m.speed = st.speed;
        m.radius = st.radius;
        m.carry_amount = st.carry_amount;
        m.mine_time = st.mine_time;
        m.gather_target = node;
        m.home_depot = depot;
        m.alive = true;
        std::uint32_t id = world.add_unit(m);

        Order o;
        o.type = OrderType::Gather;
        world.issue(id, o);

        mules_.push_back(Mule{id, lifetime});
        return id;
    }

    int active_count() const {
        int n = 0;
        for (const Mule& m : mules_)
            if (m.active) ++n;
        return n;
    }

    void advance(World& world, Fixed dt) {
        for (Mule& m : mules_) {
            if (!m.active) continue;
            m.remaining = m.remaining - dt;
            if (m.remaining.raw() <= 0) {
                m.active = false;
                if (m.id < world.units().size()) world.unit(m.id).alive = false;
            }
        }
    }

private:
    struct Mule {
        std::uint32_t id = kNoTarget;
        Fixed remaining;
        bool active = true;
    };
    std::vector<Mule> mules_;
};

}
