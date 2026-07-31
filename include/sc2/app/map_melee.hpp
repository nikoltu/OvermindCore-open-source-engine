#pragma once

#include <cstdint>
#include <vector>

#include "sc2/ai/combat.hpp"
#include "sc2/app/map_economy.hpp"
#include "sc2/formats/map_archive.hpp"
#include "sc2/sim/world.hpp"

namespace sc2::app {

struct MapMelee {
    MapEconomy eco;
    ai::CombatController ai0, ai1;
    int army_cost = 50;
    int army_cap = 24;
    sim::Fixed build_time = sim::Fixed::from_int(2);

    sim::World& world() { return eco.world; }

    std::vector<std::uint32_t> army_of(int player) const {
        std::vector<std::uint32_t> a;
        const auto& us = eco.world.units();
        for (std::uint32_t i = 0; i < us.size(); ++i) {
            const sim::Unit& u = us[i];
            if (u.alive && u.player == player && u.speed.raw() > 0 &&
                u.gather_target == sim::kNoTarget)
                a.push_back(i);
        }
        return a;
    }

    void update() {
        produce(0, eco.hall0);
        produce(1, eco.hall1);
        fight(0, ai0, eco.hall0, eco.hall1);
        fight(1, ai1, eco.hall1, eco.hall0);
    }

private:
    static sim::Unit army_proto(int player) {
        sim::Unit u;
        u.player = player;
        u.hp = u.hp_max = sim::Fixed::from_int(45);
        u.wpn_damage = sim::Fixed::from_int(6);
        u.wpn_period = sim::Fixed::from_int(1);
        u.wpn_range = sim::Fixed::from_int(5);
        u.speed = sim::Fixed::from_int(3);
        u.radius = sim::Fixed::from_ratio(1, 2);
        u.alive = true;
        return u;
    }

    void produce(int player, std::uint32_t hall) {
        if (hall >= eco.world.units().size() || !eco.world.units()[hall].alive) return;
        if (int(army_of(player).size()) >= army_cap) return;
        if (eco.world.player(player).minerals < army_cost) return;
        eco.world.queue_train(hall, army_proto(player), army_cost, 0, build_time);
    }

    void fight(int player, ai::CombatController& ai, std::uint32_t own_hall,
               std::uint32_t enemy_hall) {
        const auto& us = eco.world.units();
        if (own_hall >= us.size()) return;
        const sim::Fixed hx = us[own_hall].x, hy = us[own_hall].y;
        sim::Fixed ex = hx, ey = hy;
        if (enemy_hall < us.size()) { ex = us[enemy_hall].x; ey = us[enemy_hall].y; }

        std::vector<std::uint32_t> army = army_of(player);
        if (army.empty()) return;
        if (ai.command(eco.world, army, player, hx, hy) == ai::CombatController::Stance::Idle) {
            for (std::uint32_t id : army) {
                const sim::Unit& u = eco.world.units()[id];
                if (!u.alive) continue;
                if (u.orders.empty() || u.orders.front().type != sim::OrderType::AttackMove)
                    eco.world.issue(id, sim::Order{sim::OrderType::AttackMove, ex, ey,
                                                   sim::kNoTarget});
            }
        }
    }
};

inline MapMelee setup_melee_on_map(const formats::LoadedMap& map, int workers_per = 6,
                                   int patches = 8) {
    MapMelee m;
    m.eco = setup_economy_on_map(map, workers_per, patches);
    return m;
}

}
