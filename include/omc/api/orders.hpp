#pragma once

#include <cstdint>
#include <string>

#include "omc/api/truth_pack.hpp"
#include "omc/app/melee_game.hpp"
#include "omc/sim/world.hpp"

namespace SC2APIProtocol {
class Unit;
}

namespace omc::api {

const char* protocol_ability_name(sim::OrderType type);

std::uint32_t general_ability_id(const AbilityPack& pack, std::uint32_t ability_id);

std::uint32_t general_ability_id_of(const AbilityPack& pack, sim::OrderType type);

bool order_type_of_ability(const AbilityPack& pack, std::uint32_t ability_id, bool targets_point,
                           sim::OrderType& out);

bool is_stop_ability(const AbilityPack& pack, std::uint32_t ability_id);

enum class TargetForm { None, Point, UnitTag };
TargetForm required_target_form(sim::OrderType type);

void fill_unit_orders(const app::MeleeGame& game, const sim::Unit& unit,
                      const AbilityPack& abilities, SC2APIProtocol::Unit& out);

}
