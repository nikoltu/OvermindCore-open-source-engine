#pragma once

#include <cstdint>
#include <vector>

#include "sc2/formats/map_archive.hpp"
#include "sc2/sim/world.hpp"

namespace sc2::app {

struct MapEconomy {
    sim::World world;
    std::uint32_t hall0 = sim::kNoTarget, hall1 = sim::kNoTarget;
    std::vector<std::uint32_t> workers0, workers1, minerals0, minerals1;
};

namespace detail {
inline int first_walkable(const formats::LoadedMap& map, bool from_end, int& ox, int& oy) {
    if (!from_end) {
        for (std::uint32_t y = 0; y < map.height; ++y)
            for (std::uint32_t x = 0; x < map.width; ++x)
                if (map.terrain.walkable(x, y)) { ox = int(x); oy = int(y); return 1; }
    } else {
        for (int y = int(map.height) - 1; y >= 0; --y)
            for (int x = int(map.width) - 1; x >= 0; --x)
                if (map.terrain.walkable(std::uint32_t(x), std::uint32_t(y))) {
                    ox = x; oy = y; return 1;
                }
    }
    ox = 1; oy = 1;
    return 0;
}
}

inline MapEconomy setup_economy_on_map(const formats::LoadedMap& map, int workers_per = 6,
                                       int patches = 8, int minerals_per_patch = 1500) {
    MapEconomy e;
    e.world.nav() = map.nav;

    auto cx = [](int c) { return sim::Fixed::from_int(c) + sim::Fixed::from_ratio(1, 2); };

    auto build_base = [&](int player, int sx, int sy, std::uint32_t& hall,
                          std::vector<std::uint32_t>& workers,
                          std::vector<std::uint32_t>& minerals) {
        sim::Unit h;
        h.player = player;
        h.x = cx(sx);
        h.y = cx(sy);
        h.hp = h.hp_max = sim::Fixed::from_int(1500);
        h.radius = sim::Fixed::from_int(2);
        h.speed = sim::Fixed();
        h.alive = true;
        hall = e.world.add_unit(h);

        for (int i = 0; i < patches; ++i) {
            sim::Unit m;
            m.player = -1;
            m.x = cx(sx + 3 + (i % 4));
            m.y = cx(sy + 1 + (i / 4));
            m.resource_left = minerals_per_patch;
            m.radius = sim::Fixed::from_ratio(1, 1);
            m.alive = true;
            minerals.push_back(e.world.add_unit(m));
        }

        for (int i = 0; i < workers_per; ++i) {
            sim::Unit w;
            w.player = player;
            w.x = cx(sx + 1);
            w.y = cx(sy + 1);
            w.hp = w.hp_max = sim::Fixed::from_int(45);
            w.speed = sim::Fixed::from_int(3);
            w.radius = sim::Fixed::from_ratio(1, 2);
            w.carry_amount = 5;
            w.mine_time = sim::Fixed::from_ratio(1, 2);
            w.gather_target = minerals[std::size_t(i) % minerals.size()];
            w.home_depot = hall;
            w.alive = true;
            std::uint32_t id = e.world.add_unit(w);
            e.world.issue(id, sim::Order{sim::OrderType::Gather});
            workers.push_back(id);
        }
    };

    int s0x, s0y, s1x, s1y;
    detail::first_walkable(map, false, s0x, s0y);
    detail::first_walkable(map, true, s1x, s1y);
    build_base(0, s0x, s0y, e.hall0, e.workers0, e.minerals0);
    build_base(1, s1x, s1y, e.hall1, e.workers1, e.minerals1);
    return e;
}

}
