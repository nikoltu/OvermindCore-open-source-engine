#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "sc2/ai/melee_ai.hpp"
#include "sc2/sim/world.hpp"

namespace sc2::app {

constexpr int kStartingWorkers = 12;

struct MeleePos {
    sim::Fixed x, y;
};

struct MeleePlayerSetup {
    MeleePos start;
    sim::Unit town_hall;
    sim::Unit worker;
    int worker_count = 0;
    int worker_mineral_cost = 50;
    sim::Fixed worker_build_time = sim::Fixed::from_int(12);
    sim::Unit mineral_field;
    std::vector<MeleePos> mineral_positions;
    sim::Unit geyser_field;
    std::vector<MeleePos> geyser_positions;
    int gas_worker_count = 0;
    sim::Unit supply_unit;
    int supply_mineral_cost = 100;
    sim::Fixed supply_build_time = sim::Fixed::from_int(21);
    sim::Unit producer;
    int producer_mineral_cost = 150;
    int producer_gas_cost = 0;
    sim::Fixed producer_build_time = sim::Fixed::from_int(46);
    sim::Unit tech_unit;
    int tech_mineral_cost = 0;
    int tech_gas_cost = 0;
    sim::Fixed tech_build_time = sim::Fixed::from_int(25);
    bool has_tech = false;
    sim::Unit army_unit;
    int army_mineral_cost = 50;
    int army_gas_cost = 0;
    sim::Fixed army_build_time = sim::Fixed::from_int(1);
    int army_count = 1;
    bool has_army_unit2 = false;
    sim::Unit army_unit2;
    int army2_mineral_cost = 50;
    int army2_gas_cost = 0;
    sim::Fixed army2_build_time = sim::Fixed::from_int(1);
    std::string army_unit2_gate;
    std::vector<ai::BuildItem> tech_chain;
    int start_minerals = 50;
    int army_threshold = 3;
};

struct MeleeSetup {
    std::vector<MeleePlayerSetup> players;
    std::vector<sim::Unit> neutrals;
};

struct MeleePlayerLayout {
    uint32_t town_hall = sim::kNoTarget;
    std::vector<uint32_t> workers;
    std::vector<uint32_t> minerals;
    std::vector<uint32_t> geysers;
    uint32_t barracks = sim::kNoTarget;
    ai::MeleeConfig config;
    ai::MeleeBase base;
};

struct MeleeLayout {
    std::vector<MeleePlayerLayout> players;
    std::vector<std::string> unit_type;
};

MeleeLayout build_melee(sim::World& w, const MeleeSetup& s);

}
