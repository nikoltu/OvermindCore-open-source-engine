#include "sc2/api/orders.hpp"

#include "s2clientprotocol/sc2api.pb.h"

namespace sc2::api {

const char* protocol_ability_name(sim::OrderType type) {
    switch (type) {
        case sim::OrderType::Move: return "Move";
        case sim::OrderType::Attack:
        case sim::OrderType::AttackMove: return "Attack";
        case sim::OrderType::HoldPosition: return "HoldPosition";
        case sim::OrderType::Patrol: return "Patrol";

        case sim::OrderType::Idle: return nullptr;

        case sim::OrderType::Repair:
        case sim::OrderType::Gather:
        case sim::OrderType::Cast:
        case sim::OrderType::Load:
        case sim::OrderType::Unload: return nullptr;
    }
    return nullptr;
}

std::uint32_t general_ability_id(const AbilityPack& pack, std::uint32_t ability_id) {
    if (!pack.loaded) return 0;
    const auto edge = pack.remaps_to.find(ability_id);
    return edge == pack.remaps_to.end() ? ability_id : edge->second;
}

std::uint32_t general_ability_id_of(const AbilityPack& pack, sim::OrderType type) {
    if (!pack.loaded) return 0;
    const char* name = protocol_ability_name(type);
    if (!name) return 0;
    const auto row = pack.id_by_name.find(name);
    if (row == pack.id_by_name.end()) return 0;
    return general_ability_id(pack, row->second);
}

bool order_type_of_ability(const AbilityPack& pack, std::uint32_t ability_id, bool targets_point,
                           sim::OrderType& out) {
    if (!pack.loaded) return false;
    const std::uint32_t want = general_ability_id(pack, ability_id);
    if (!want) return false;
    static const sim::OrderType kNameable[] = {sim::OrderType::Move, sim::OrderType::Attack,
                                               sim::OrderType::HoldPosition,
                                               sim::OrderType::Patrol};
    int hits = 0;
    sim::OrderType found = sim::OrderType::Idle;
    for (const sim::OrderType verb : kNameable) {
        if (general_ability_id_of(pack, verb) != want) continue;
        found = verb;
        ++hits;
    }
    if (hits != 1) return false;
    out = (found == sim::OrderType::Attack && targets_point) ? sim::OrderType::AttackMove : found;
    return true;
}

TargetForm required_target_form(sim::OrderType type) {
    switch (type) {
        case sim::OrderType::Move:
        case sim::OrderType::AttackMove:
        case sim::OrderType::Patrol: return TargetForm::Point;
        case sim::OrderType::Attack: return TargetForm::UnitTag;
        default: return TargetForm::None;
    }
}

constexpr const char* kStopAbility = "Stop";

bool is_stop_ability(const AbilityPack& pack, std::uint32_t ability_id) {
    if (!pack.loaded) return false;
    const auto row = pack.id_by_name.find(kStopAbility);
    if (row == pack.id_by_name.end()) return false;
    const std::uint32_t general = general_ability_id(pack, ability_id);
    return general != 0 && general == general_ability_id(pack, row->second);
}

namespace {

bool carries_point(sim::OrderType type) {
    switch (type) {
        case sim::OrderType::Move:
        case sim::OrderType::AttackMove:
        case sim::OrderType::Patrol:
        case sim::OrderType::Cast: return true;
        default: return false;
    }
}

}

void fill_unit_orders(const app::MeleeGame& game, const sim::Unit& unit,
                      const AbilityPack& abilities, SC2APIProtocol::Unit& out) {
    for (const sim::Order& o : unit.orders) {
        if (o.type == sim::OrderType::Idle) continue;

        SC2APIProtocol::UnitOrder* row = out.add_orders();

        const std::uint32_t published = general_ability_id_of(abilities, o.type);
        if (published) row->set_ability_id(published);

        if (o.target != sim::kNoTarget) {
            row->set_target_unit_tag(std::uint64_t(o.target));
        } else if (carries_point(o.type)) {
            SC2APIProtocol::Point* p = row->mutable_target_world_space_pos();
            const float x = float(o.tx.to_double());
            const float y = float(o.ty.to_double());
            p->set_x(x);
            p->set_y(y);
            p->set_z(game.ground_z_at(x, y));
        }

    }
}

}
