#pragma once

#include <string>

#include "sc2/app/map_melee_setup.hpp"
#include "sc2/app/sandbox_game.hpp"

namespace sc2::app {

struct RaceUnits {
    const char* hall;
    const char* worker;
    const char* producer;
    const char* supply;
    const char* army;
};

inline RaceUnits race_units(Race r) {
    switch (r) {
        case Race::Protoss: return {"Nexus", "Probe", "Gateway", "Pylon", "Zealot"};
        case Race::Zerg:    return {"Hatchery", "Drone", "SpawningPool", "Overlord", "Zergling"};
        case Race::Terran:
        default:            return {"CommandCenter", "SCV", "Barracks", "SupplyDepot", "Marine"};
    }
}

inline MeleeProtos race_protos(const data::Catalog& cat, Race r) {
    using sim::Fixed;
    const RaceUnits u = race_units(r);
    MeleeProtos pr;
    pr.hall = make_unit(cat, u.hall, 0);
    pr.hall.speed = Fixed();
    pr.worker = make_unit(cat, u.worker, 0);
    pr.worker.is_worker = true;
    pr.producer = make_unit(cat, u.producer, 0);
    pr.producer.speed = Fixed();
    pr.supply = make_unit(cat, u.supply, 0);
    pr.supply.speed = Fixed();
    pr.army = make_unit(cat, u.army, 0);
    pr.mineral = make_unit(cat, "MineralField", -1);
    pr.mineral.player = -1;
    pr.mineral.speed = Fixed();
    pr.mineral.radius = Fixed::from_int(1);
    pr.mineral.resource_left = 1500;
    pr.geyser = make_unit(cat, "VespeneGeyser", -1);
    pr.geyser.player = -1;
    pr.geyser.speed = Fixed();
    pr.geyser.is_gas = true;
    pr.geyser.radius = Fixed::from_int(1);
    pr.geyser.resource_left = 2250;
    return pr;
}

inline void apply_race_costs(MeleePlayerSetup& p, const data::Catalog& cat, Race r,
                             data::Production& prod) {
    using sim::Fixed;
    const RaceUnits u = race_units(r);
    auto secs = [&](const char* id, int fallback) {
        double t = prod.time_of(id);
        return t > 0.0 ? Fixed::from_raw(int(t * double(Fixed::kOne) + 0.5)) : Fixed::from_int(fallback);
    };
    const data::UnitCost wcost = data::unit_cost(cat, u.worker);
    const data::UnitCost pcost = data::unit_cost(cat, u.producer);
    const data::UnitCost dcost = data::unit_cost(cat, u.supply);
    const data::UnitCost acost = data::unit_cost(cat, u.army);
    p.worker_mineral_cost = wcost.minerals > 0 ? wcost.minerals : 50;
    p.worker_build_time = secs(u.worker, 12);
    p.supply_mineral_cost = dcost.minerals > 0 ? dcost.minerals : 100;
    p.supply_build_time = secs(u.supply, 21);
    p.producer_mineral_cost = pcost.minerals > 0 ? pcost.minerals : 150;
    p.producer_gas_cost = pcost.gas;
    p.producer_build_time = secs(u.producer, 46);
    p.army_mineral_cost = acost.minerals > 0 ? acost.minerals : 50;
    p.army_gas_cost = acost.gas;
    p.army_build_time = secs(u.army, 18);
    p.army_count = prod.train_count(u.army);
    p.army_threshold = 5;
}

inline MeleeSetup melee_setup_for_map(const formats::LoadedMap& map, const data::Catalog& cat,
                                      Race r0, Race r1, int worker_count = 12, int patches = 8) {
    const MapMeleeStarts st = map_melee_starts(map);
    data::Production prod;
    prod.build(cat);

    MeleeSetup s;
    const Race races[2] = {r0, r1};
    for (int side = 0; side < 2; ++side) {
        const Race r = races[side];
        const MeleeProtos pr = race_protos(cat, r);
        const int sx = side ? st.s1x : st.s0x, sy = side ? st.s1y : st.s0y;
        MeleePlayerSetup p = map_melee_player(pr, sx, sy, st.cx, st.cy, worker_count, patches);
        apply_race_costs(p, cat, r, prod);
        s.players.push_back(p);
    }
    return s;
}

}
