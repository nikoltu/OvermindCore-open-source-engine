#pragma once

#include <string>

#include "omc/data/catalog.hpp"
#include "omc/data/production.hpp"
#include "omc/data/requirements.hpp"
#include "omc/sim/fixed.hpp"

namespace omc::data {

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
