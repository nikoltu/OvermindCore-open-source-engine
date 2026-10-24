#pragma once

#include <string>

#include "sc2/data/catalog.hpp"
#include "sc2/data/production.hpp"
#include "sc2/data/requirements.hpp"
#include "sc2/sim/fixed.hpp"

namespace sc2::data {

struct UnitCost {
    int minerals = 0;
    int gas = 0;
    sim::Fixed supply;
};

UnitCost unit_cost(const Catalog& cat, const std::string& unit_id);
UnitCost build_charge(const Catalog& cat, const std::string& structure, std::string builder = {},
                      std::string abil = {});
sim::Fixed supply_provided(const Catalog& cat, const std::string& unit_id);

struct PlayerEconomy {
    int minerals = 50;
    int gas = 0;
    sim::Fixed supply_used;
    sim::Fixed supply_max;
};

bool can_afford(const Catalog& cat, const PlayerEconomy& eco,
                const std::string& unit_id);

bool try_train(const Catalog& cat, const Production& prod, PlayerEconomy& eco,
               const TechState& tech, const std::string& ability,
               const std::string& product);

}
