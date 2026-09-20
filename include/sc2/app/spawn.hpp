#pragma once

#include <string>
#include <vector>

#include "sc2/data/catalog.hpp"
#include "sc2/data/upgrades.hpp"
#include "sc2/sim/world.hpp"

namespace sc2::app {

inline std::vector<std::pair<sim::Fixed, sim::Fixed>> spawn_slots_of(const data::Catalog::SpawnBehavior& sb) {
    std::vector<std::pair<sim::Fixed, sim::Fixed>> slots;
    slots.reserve(sb.offsets.size());
    for (const auto& [ox, oy] : sb.offsets) slots.emplace_back(sim::Fixed::from_double(ox), sim::Fixed::from_double(oy));
    return slots;
}

void weapon_attribute_bonus(const data::Catalog& cat, const std::string& weapon_id,
                            std::uint32_t& bits, sim::Fixed& amount,
                            const data::UpgradeCatalog* up = nullptr,
                            const std::vector<std::string>* researched = nullptr);

sim::BuildVerb worker_build_verb(const data::Catalog& cat, const std::string& worker_unit_id);

sim::Unit make_unit(const data::Catalog& cat, const std::string& unit_id,
                    int player, sim::Fixed x = sim::Fixed(),
                    sim::Fixed y = sim::Fixed());

inline constexpr float kPlacedStructureFacing = 3.9253f;
inline constexpr float kUnauthoredFacing = 4.712389f;

inline float placed_facing(bool structure, bool resource, float rotation,
                           float structure_facing = kPlacedStructureFacing) {
    if (structure || resource) return structure_facing;
    const float kTwoPi = 6.2831853f, kHalfPi = 1.5707963f;
    float f = rotation - kHalfPi;
    while (f < 0.0f) f += kTwoPi;
    while (f >= kTwoPi) f -= kTwoPi;
    return f;
}

}
