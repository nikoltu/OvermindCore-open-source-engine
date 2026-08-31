#pragma once

#include <string>

#include "sc2/app/map_melee_setup.hpp"
#include "sc2/app/sandbox_game.hpp"
#include "sc2/sim/weapon.hpp"

namespace sc2::app {

struct RaceUnits {
    const char* hall;
    const char* worker;
    const char* producer;
    const char* supply;
    const char* army;
    const char* tech;
};

inline RaceUnits race_units(Race r) {
    switch (r) {
        case Race::Protoss: return {"Nexus", "Probe", "Gateway", "Pylon", "Zealot", "Forge"};
        case Race::Zerg:    return {"Hatchery", "Drone", "SpawningPool", "Overlord", "Zergling",
                                    "EvolutionChamber"};
        case Race::Terran:
        default:            return {"CommandCenter", "SCV", "Barracks", "SupplyDepot", "Marine",
                                    "EngineeringBay"};
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
    pr.geyser = make_unit(cat, "VespeneGeyser", -1);
    pr.geyser.player = -1;
    pr.geyser.speed = Fixed();
    pr.geyser.is_gas = true;
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
    const data::UnitCost tcost = data::unit_cost(cat, u.tech);
    p.tech_unit = make_unit(cat, u.tech, 0);
    p.tech_unit.speed = Fixed();
    p.tech_mineral_cost = tcost.minerals > 0 ? tcost.minerals : 125;
    p.tech_gas_cost = tcost.gas;
    p.tech_build_time = secs(u.tech, 25);
    p.has_tech = true;

    const char* caster = nullptr;
    const char* gate = nullptr;
    const char* chain[3] = {nullptr, nullptr, nullptr};
    bool chain_morph[3] = {false, false, false};
    bool chain_addon[3] = {false, false, false};
    if (r == Race::Protoss) {
        caster = "HighTemplar";
        gate = "TemplarArchive";
        chain[0] = "CyberneticsCore";
        chain[1] = "TwilightCouncil";
        chain[2] = "TemplarArchive";
    } else if (r == Race::Zerg) {
        caster = "Infestor";
        gate = "InfestationPit";
        chain[0] = "Lair";           chain_morph[0] = true;
        chain[1] = "InfestationPit"; chain_morph[1] = false;
    } else if (r == Race::Terran) {
        caster = "Ghost";
        gate = "GhostAcademy";
        chain[0] = "BarracksTechLab"; chain_addon[0] = true;
        chain[1] = "GhostAcademy";    chain_addon[1] = false;
    }
    if (caster) {
        const data::UnitCost ccost = data::unit_cost(cat, caster);
        p.has_army_unit2 = true;
        p.army_unit2 = make_unit(cat, caster, 0);
        p.army2_mineral_cost = ccost.minerals > 0 ? ccost.minerals : 50;
        p.army2_gas_cost = ccost.gas;
        p.army2_build_time = secs(caster, 55);
        p.army_unit2_gate = gate;
        for (int i = 0; i < 3 && chain[i]; ++i) {
            const data::UnitCost bc = data::unit_cost(cat, chain[i]);
            ai::BuildItem b;
            b.proto = make_unit(cat, chain[i], 0);
            b.proto.speed = Fixed();
            b.minerals = bc.minerals > 0 ? bc.minerals : 150;
            b.gas = bc.gas;
            b.build_time = secs(chain[i], 40);
            b.morph_self = chain_morph[i];
            b.addon_self = chain_addon[i];
            p.tech_chain.push_back(b);
        }
    }
}

inline void snap_base_to_real_resources(const formats::LoadedMap& map, const data::Catalog& cat,
                                        int sx, int sy, MeleePlayerSetup& p,
                                        std::vector<char>& consumed, double radius = 14.0) {
    using sim::Fixed;
    std::remove_reference_t<decltype(p.mineral_positions)> mins, gas;
    std::vector<sim::Unit> min_protos, gas_protos;
    std::vector<std::size_t> min_idx, gas_idx;
    for (std::size_t i = 0; i < map.objects.units.size(); ++i) {
        const auto& u = map.objects.units[i];
        const double dx = u.x - sx, dy = u.y - sy;
        if (dx * dx + dy * dy > radius * radius) continue;
        const bool is_gas = u.unit_type.find("Geyser") != std::string::npos ||
                            u.unit_type.find("Vespene") != std::string::npos;
        const bool is_min = !is_gas && u.unit_type.find("Mineral") != std::string::npos;
        if (!is_gas && !is_min) continue;
        sim::Unit proto;
        try {
            proto = make_unit(cat, u.unit_type, -1);
        } catch (const std::exception&) {
            proto = is_gas ? p.geyser_field : p.mineral_field;
        }
        proto.player = -1;
        proto.speed = Fixed();
        proto.is_gas = is_gas;
        if (is_gas) {
            gas.push_back({Fixed::from_double(u.x), Fixed::from_double(u.y)});
            gas_protos.push_back(std::move(proto));
            gas_idx.push_back(i);
        } else {
            mins.push_back({Fixed::from_double(u.x), Fixed::from_double(u.y)});
            min_protos.push_back(std::move(proto));
            min_idx.push_back(i);
        }
    }
    if (mins.size() >= 4) {
        p.mineral_positions = std::move(mins);
        p.mineral_protos = std::move(min_protos);
        for (std::size_t i : min_idx) consumed[i] = 1;
    }
    if (!gas.empty()) {
        p.geyser_positions = std::move(gas);
        p.geyser_protos = std::move(gas_protos);
        for (std::size_t i : gas_idx) consumed[i] = 1;
    }
}

inline MeleeSetup melee_setup_for_map(const formats::LoadedMap& map, const data::Catalog& cat,
                                      Race r0, Race r1, int worker_count = 12, int patches = 8) {
    const MapMeleeStarts st = map_melee_starts(map);
    data::Production prod;
    prod.build(cat);

    MeleeSetup s;
    const Race races[2] = {r0, r1};
    std::vector<char> consumed(map.objects.units.size(), 0);
    for (int side = 0; side < 2; ++side) {
        const Race r = races[side];
        const MeleeProtos pr = race_protos(cat, r);
        const int sx = side ? st.s1x : st.s0x, sy = side ? st.s1y : st.s0y;
        MeleePlayerSetup p = map_melee_player(pr, sx, sy, st.cx, st.cy, worker_count, patches);
        snap_base_to_real_resources(map, cat, sx, sy, p, consumed);
        apply_race_costs(p, cat, r, prod);
        s.players.push_back(p);
    }

    {
        using sim::Fixed;
        const MeleeProtos npr = race_protos(cat, r0);
        for (std::size_t ui = 0; ui < map.objects.units.size(); ++ui) {
            const auto& u = map.objects.units[ui];
            if (consumed[ui]) continue;
            const bool gas = u.unit_type.find("Geyser") != std::string::npos ||
                             u.unit_type.find("Vespene") != std::string::npos;
            const bool min = !gas && u.unit_type.find("Mineral") != std::string::npos;
            sim::Unit n;
            if (gas || min) {
                try {
                    n = make_unit(cat, u.unit_type, -1);
                } catch (const std::exception&) {
                    n = gas ? npr.geyser : npr.mineral;
                }
                n.is_gas = gas;
                n.speed = Fixed();
            } else {
                try {
                    n = make_unit(cat, u.unit_type, -1);
                } catch (const std::exception&) {
                    continue;
                }
                n.speed = Fixed();
            }
            n.x = Fixed::from_double(u.x);
            n.y = Fixed::from_double(u.y);
            n.player = u.player > 0 ? u.player - 1 : -1;
            n.alive = true;
            s.neutral_facing.push_back(placed_facing(
                (n.attributes & sim::kAttrStructure) != 0, gas || min || n.resource_left > 0,
                u.rotation));
            s.neutrals.push_back(std::move(n));
        }
    }
    return s;
}

}
