#pragma once

#include <optional>
#include <string>
#include <vector>

#include "sc2/data/catalog.hpp"
#include "sc2/data/upgrades.hpp"
#include "sc2/sim/ability_system.hpp"
#include "sc2/sim/mule.hpp"
#include "sc2/sim/repair.hpp"
#include "sc2/sim/sensor_tower.hpp"
#include "sc2/sim/supply_drop.hpp"
#include "sc2/sim/world.hpp"

namespace sc2::app {

double weapon_upgrade_bonus(const data::Catalog& cat, const data::UpgradeCatalog& up,
                            const std::string& weapon_id,
                            const std::vector<std::string>& researched);

double armor_upgrade_bonus(const data::UpgradeCatalog& up, const std::string& unit_name,
                           const std::vector<std::string>& researched);

void apply_unit_upgrades(const data::Catalog& cat, const data::UpgradeCatalog& up,
                         const std::string& unit_name, const std::vector<std::string>& researched,
                         sim::Unit& u);

sim::Unit make_unit_upgraded(const data::Catalog& cat, const data::UpgradeCatalog& up,
                             const std::string& unit_id, int player,
                             const std::vector<std::string>& researched);

struct AbilityBuild {
    sim::AbilityDef def;
    sim::Fixed energy_cost;
    sim::Fixed cooldown;
    std::uint32_t req_attr_mask = 0;
    std::uint32_t exc_attr_mask = 0;
    bool require_energy = false;
    std::vector<sim::VitalGate> vital_gates;
    std::string spawn_unit;
    sim::Fixed spawn_life;
};

std::optional<AbilityBuild> make_ability(const data::Catalog& cat, const std::string& abil_id);

std::pair<int, int> morph_cost_delta(const data::Catalog& cat, const std::string& source_unit,
                                     const std::string& target_unit);

std::uint32_t calldown_mule(sim::World& world, sim::MuleSystem& mules, std::uint32_t caster,
                            sim::Fixed tx, sim::Fixed ty);

bool order_repair(const data::Catalog& cat, sim::World& world, sim::RepairSystem& repairs,
                  std::uint32_t repairer, std::uint32_t target, const std::string& target_unit_id);

int add_sensor_tower(const data::Catalog& cat, sim::SensorTowerSystem& towers, std::int32_t owner,
                     sim::Fixed x, sim::Fixed y, const std::string& unit_id);

bool calldown_supply_drop(sim::World& world, sim::SupplyDropSystem& drops, std::uint32_t caster,
                          std::uint32_t depot);

bool cast_abduct(sim::World& world, std::uint32_t caster, std::uint32_t target);

}
