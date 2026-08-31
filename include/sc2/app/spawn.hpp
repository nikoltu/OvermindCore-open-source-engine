#pragma once

#include <string>
#include <vector>

#include "sc2/data/catalog.hpp"
#include "sc2/data/upgrades.hpp"
#include "sc2/sim/world.hpp"

namespace sc2::app {

void weapon_attribute_bonus(const data::Catalog& cat, const std::string& weapon_id,
                            std::uint32_t& bits, sim::Fixed& amount,
                            const data::UpgradeCatalog* up = nullptr,
                            const std::vector<std::string>* researched = nullptr);

sim::Unit make_unit(const data::Catalog& cat, const std::string& unit_id,
                    int player, sim::Fixed x = sim::Fixed(),
                    sim::Fixed y = sim::Fixed());

inline constexpr float kPlacedStructureFacing = 3.9253f;

inline float placed_facing(bool structure, bool resource, float rotation) {
    if (resource) return 0.0f;
    if (structure) return kPlacedStructureFacing;
    const float kTwoPi = 6.2831853f, kHalfPi = 1.5707963f;
    float f = rotation - kHalfPi;
    while (f < 0.0f) f += kTwoPi;
    while (f >= kTwoPi) f -= kTwoPi;
    return f;
}

}
