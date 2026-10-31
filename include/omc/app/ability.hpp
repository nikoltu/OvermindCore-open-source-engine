#pragma once

#include <optional>
#include <string>
#include <vector>

#include "omc/data/catalog.hpp"
#include "omc/data/upgrades.hpp"
#include "omc/sim/ability_system.hpp"
#include "omc/sim/mule.hpp"
#include "omc/sim/scan_sweep.hpp"
#include "omc/sim/sensor_tower.hpp"
#include "omc/sim/supply_drop.hpp"
#include "omc/sim/world.hpp"

namespace omc::app {

double weapon_upgrade_bonus(const data::Catalog& cat, const data::UpgradeCatalog& up,
                            const std::string& weapon_id,
                            const std::vector<std::string>& researched);

std::vector<std::string> army_weapon_upgrade_ids(const data::Catalog& cat, const data::UpgradeCatalog& up,
                                                 const std::string& army_unit);

std::vector<std::string> army_armor_upgrade_ids(const data::UpgradeCatalog& up,
                                                const std::string& army_unit);

std::vector<std::string> army_shield_upgrade_ids(const data::UpgradeCatalog& up,
                                                 const std::string& army_unit);

double armor_upgrade_bonus(const data::UpgradeCatalog& up, const std::string& unit_name,
                           const std::vector<std::string>& researched);

void apply_unit_upgrades(const data::Catalog& cat, const data::UpgradeCatalog& up,
                         const std::string& unit_name, const std::vector<std::string>& researched,
                         sim::Unit& u);

sim::Unit make_unit_upgraded(const data::Catalog& cat, const data::UpgradeCatalog& up,
                             const std::string& unit_id, int player,
                             const std::vector<std::string>& researched);

struct UpgradeRevert {
    std::uint32_t unit_id;
    sim::Fixed wpn_damage, wpn2_damage, armor, speed, shield_armor, hp_max, hp, energy_max, energy,
        wpn_range, wpn2_range, wpn_period, wpn2_period, wpn_bonus_amount,
        shields_max, shields, acquire_range, speed_mult_creep, burrow_speed;
    std::uint32_t wpn_bonus_attr;
    sim::Fixed accel{};
};

std::size_t apply_upgrade_to_live(sim::World& w, const data::Catalog& cat,
                                  const data::UpgradeCatalog& up, const std::string& upgrade_id,
                                  int player, std::vector<UpgradeRevert>* revert = nullptr, bool units = true);
std::size_t fold_lagging_upgrades(sim::World& w, const data::Catalog& cat, const data::UpgradeCatalog& up);
bool fold_unit_lagging(const sim::World& w, const data::Catalog& cat, const data::UpgradeCatalog& up, sim::Unit& u);

void revert_live_upgrade(sim::World& w, std::vector<UpgradeRevert>& revert, std::size_t count);

struct AbilityBuild {
    sim::AbilityDef def;
    sim::Fixed energy_cost;
    sim::Fixed life_cost;
    sim::Fixed cooldown;
    sim::Fixed cast_intro_time;
    sim::Fixed cast_finish_time;
    sim::Fixed cast_outro_time;
    std::uint32_t req_attr_mask = 0;
    std::uint32_t exc_attr_mask = 0;
    bool require_energy = false;
    bool require_energy_pool = false;
    bool requires_ground = false;
    bool requires_spawner_target = false;
    std::vector<sim::VitalGate> vital_gates;
    std::vector<sim::VitalGate> autocast_caster_gates;
    std::vector<std::uint32_t> autocast_not_behaviors;
    std::string spawn_unit;
    sim::Fixed spawn_life;
    sim::Fixed spawn_dmg_fraction{sim::Fixed::from_int(1)};
    bool spawn_no_damage = false;
    bool spawn_requires_creep = false;
    std::vector<sim::EffectNode> tree;
    int32_t tree_root = -1;
    bool from_effect_tree = false;
    bool target_rel_authored = false;
    std::uint8_t target_exc_rel = 0;
    bool full_tree = false;
    std::vector<std::string> unbuilt;
    std::vector<std::string> tolerated;
    std::vector<std::pair<int32_t, std::string>> tree_spawns;
    std::vector<int32_t> tree_salvage;
};

std::vector<std::string> effect_tree_gaps(const data::Catalog& cat, const std::string& abil_id);

int32_t build_effect(const data::Catalog& cat, const std::string& effect_id, AbilityBuild& b);

int32_t register_ability(sim::World& world, const AbilityBuild& b);

std::optional<AbilityBuild> make_ability(const data::Catalog& cat, const std::string& abil_id);
std::optional<AbilityBuild> make_ability_flat(const data::Catalog& cat, const std::string& abil_id);
std::optional<AbilityBuild> make_ability_ai(const data::Catalog& cat, const std::string& abil_id);
void parse_ability_autocast_validators(const data::Catalog& cat, const std::string& abil_id, AbilityBuild& b);

sim::Fixed ability_energy_cost(const data::Catalog& cat, const std::string& abil_id);
sim::Fixed ability_cooldown_sec(const data::Catalog& cat, const std::string& abil_id);

enum class AbilTargetMode { Instant, Unit, Point };
AbilTargetMode ability_target_mode(const data::Catalog& cat, const std::string& abil_id);
std::uint8_t ability_target_excluded_rel(const data::Catalog& cat, const std::string& abil_id, bool& authored);

std::pair<int, int> morph_cost_delta(const data::Catalog& cat, const std::string& source_unit,
                                     const std::string& target_unit);
std::pair<int, int> morph_charge(const data::Catalog& cat, const std::string& source_unit, const std::string& morph_abil);

int add_sensor_tower(const data::Catalog& cat, sim::SensorTowerSystem& towers, std::int32_t owner,
                     sim::Fixed x, sim::Fixed y, const std::string& unit_id);

bool calldown_supply_drop(sim::World& world, sim::SupplyDropSystem& drops, std::uint32_t caster,
                          std::uint32_t depot);

bool cast_abduct(sim::World& world, std::uint32_t caster, std::uint32_t target);

bool is_resource_drop_off(const data::Catalog& cat, const std::string& unit_id,
                          const std::string& resource);

inline std::uint32_t behavior_id_hash(const std::string& id) {
    std::uint32_t h = 2166136261u;
    for (unsigned char c : id) {
        h ^= c;
        h *= 16777619u;
    }
    return h;
}

std::uint32_t nearest_drop_off(const data::Catalog& cat, const sim::World& world,
                               std::uint32_t worker);

bool order_return_cargo(const data::Catalog& cat, sim::World& world, std::uint32_t worker);

sim::EffectRef route_primary_weapon_through_executor(sim::World& world, std::uint32_t unit);

}
