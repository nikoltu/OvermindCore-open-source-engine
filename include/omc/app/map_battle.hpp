#pragma once

#include <cstdint>
#include <vector>

#include "omc/ai/combat.hpp"
#include "omc/formats/map_archive.hpp"
#include "omc/sim/world.hpp"

namespace omc::app {

struct MapBattle {
    sim::World world;
    std::vector<std::uint32_t> army0, army1;
    ai::CombatController ai0, ai1;
    sim::Fixed start0_x, start0_y, start1_x, start1_y;

    void micro() {
        step_side(ai0, army0, 0, start0_x, start0_y, start1_x, start1_y);
        step_side(ai1, army1, 1, start1_x, start1_y, start0_x, start0_y);
    }

private:
    void step_side(ai::CombatController& ai, const std::vector<std::uint32_t>& army, int player,
                   sim::Fixed rally_x, sim::Fixed rally_y, sim::Fixed enemy_x,
                   sim::Fixed enemy_y) {
        if (ai.command(world, army, player, rally_x, rally_y) ==
            ai::CombatController::Stance::Idle) {
            for (std::uint32_t id : army) {
                if (id >= world.units().size()) continue;
                const sim::Unit& u = world.units()[id];
                if (!u.alive || u.player != player) continue;
                if (u.orders.empty() || u.orders.front().type != sim::OrderType::AttackMove)
                    world.issue(id, sim::Order{sim::OrderType::AttackMove, enemy_x, enemy_y,
                                               sim::kNoTarget});
            }
        }
    }

public:
};

namespace detail {
inline sim::Unit battle_trooper(int player) {
    sim::Unit u;
    u.player = player;
    u.hp = u.hp_max = sim::Fixed::from_int(60);
    u.wpn_damage = sim::Fixed::from_int(6);
    u.wpn_period = sim::Fixed::from_int(1);
    u.wpn_range = sim::Fixed::from_int(5);
    u.speed = sim::Fixed::from_int(3);
    u.radius = sim::Fixed::from_ratio(1, 2);
    u.alive = true;
    return u;
}
}

inline MapBattle setup_battle_on_map(const formats::LoadedMap& map, int army_size = 6) {
    MapBattle b;
    b.world.nav() = map.nav;

    auto cell_center = [&](int cx, int cy, sim::Fixed& wx, sim::Fixed& wy) {
        wx = sim::Fixed::from_int(cx) + sim::Fixed::from_ratio(1, 2);
        wy = sim::Fixed::from_int(cy) + sim::Fixed::from_ratio(1, 2);
    };
    int s0x = 1, s0y = 1, s1x = int(map.width) - 2, s1y = int(map.height) - 2;
    for (std::uint32_t y = 0; y < map.height; ++y) {
        bool done = false;
        for (std::uint32_t x = 0; x < map.width; ++x)
            if (map.terrain.walkable(x, y)) { s0x = int(x); s0y = int(y); done = true; break; }
        if (done) break;
    }
    for (int y = int(map.height) - 1; y >= 0; --y) {
        bool done = false;
        for (int x = int(map.width) - 1; x >= 0; --x)
            if (map.terrain.walkable(std::uint32_t(x), std::uint32_t(y))) {
                s1x = x; s1y = y; done = true; break;
            }
        if (done) break;
    }
    cell_center(s0x, s0y, b.start0_x, b.start0_y);
    cell_center(s1x, s1y, b.start1_x, b.start1_y);

    auto place = [&](int player, int cx, int cy, std::vector<std::uint32_t>& army) {
        int placed = 0;
        for (int dy = 0; dy < 8 && placed < army_size; ++dy)
            for (int dx = 0; dx < 8 && placed < army_size; ++dx) {
                sim::Unit u = detail::battle_trooper(player);
                u.x = sim::Fixed::from_int(cx + dx) + sim::Fixed::from_ratio(1, 2);
                u.y = sim::Fixed::from_int(cy + dy) + sim::Fixed::from_ratio(1, 2);
                army.push_back(b.world.add_unit(u));
                ++placed;
            }
    };
    place(0, s0x, s0y, b.army0);
    place(1, s1x, s1y, b.army1);
    return b;
}

}
