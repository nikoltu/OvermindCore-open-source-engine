#include <string>
#include <unordered_map>
#include <vector>

#include "s2clientprotocol/sc2api.pb.h"

#include "sc2/api/bridge.hpp"
#include "sc2/api/orders.hpp"
#include "sc2/api/truth_pack.hpp"
#include "sc2/app/melee_game.hpp"
#include "sc2/sim/world.hpp"

namespace sc2::api::bridge {

namespace {

constexpr int kSimNeutralOwner = -1;
constexpr int kApiNeutralOwner = 16;

int api_owner(int sim_owner) {
    return sim_owner == kSimNeutralOwner ? kApiNeutralOwner : sim_owner + 1;
}

SC2APIProtocol::Alliance alliance_of(int sim_owner, int joined_sim_side) {
    if (sim_owner == kSimNeutralOwner) return SC2APIProtocol::Neutral;
    return sim_owner == joined_sim_side ? SC2APIProtocol::Self : SC2APIProtocol::Enemy;
}

constexpr float kApiFacingTurn = 6.283185307179586f;

float api_facing(double sim_facing) {
    const float f = float(sim_facing);
    return f < 0.0f ? f + kApiFacingTurn : f;
}

}

void fill_observation(Host& host, const View& view, SC2APIProtocol::ResponseObservation& out) {
    app::MeleeGame* game = host.match();
    if (!game) return;
    const sim::World& world = game->world();

    SC2APIProtocol::Observation* obs = out.mutable_observation();
    obs->set_game_loop(std::uint32_t(world.tick()));

    const int joined_sim_side = view.player - 1;
    SC2APIProtocol::PlayerCommon* common = obs->mutable_player_common();
    common->set_player_id(std::uint32_t(view.player));
    const sim::Player& p = world.player(joined_sim_side);
    common->set_minerals(std::uint32_t(p.minerals));
    common->set_vespene(std::uint32_t(p.gas));
    common->set_food_used(std::uint32_t(world.supply_used(joined_sim_side).to_double()));
    common->set_food_cap(std::uint32_t(world.supply_max(joined_sim_side).to_double()));

    const UnitTypePack pack = load_unit_type_pack(host.truth_data_dir(), host.data_build());

    const AbilityPack abilities = load_ability_pack(host.truth_data_dir(), host.data_build());

    const sim::VisionGrid& vision = game->vision();
    const bool unscoped = view.revealed || view.disable_fog;

    SC2APIProtocol::ObservationRaw* raw = obs->mutable_raw_data();
    for (const sim::Unit& u : world.units()) {
        if (!u.alive) continue;
        if (u.loaded) continue;
        if (!unscoped && u.player != joined_sim_side &&
            vision.state_at(u.x, u.y) != sim::Visibility::Visible)
            continue;
        const auto row = pack.id_by_name.find(u.catalog_id);
        if (row == pack.id_by_name.end()) continue;

        SC2APIProtocol::Unit* out_u = raw->add_units();
        out_u->set_display_type(SC2APIProtocol::Visible);
        out_u->set_alliance(alliance_of(u.player, joined_sim_side));
        out_u->set_tag(std::uint64_t(u.id));
        out_u->set_unit_type(row->second);
        out_u->set_owner(api_owner(u.player));
        SC2APIProtocol::Point* pos = out_u->mutable_pos();
        pos->set_x(float(u.x.to_double()));
        pos->set_y(float(u.y.to_double()));
        pos->set_z(game->ground_z_at(float(u.x.to_double()), float(u.y.to_double())));
        out_u->set_facing(api_facing(u.facing.to_double()));
        out_u->set_radius(float(u.radius.to_double()));
        out_u->set_build_progress(u.constructing ? float(u.build_progress.to_double()) : 1.0f);
        out_u->set_cloak(u.cloaked_effective() ? SC2APIProtocol::Cloaked
                                               : SC2APIProtocol::NotCloaked);
        out_u->set_health(float(u.hp.to_double()));
        out_u->set_health_max(float(u.hp_max.to_double()));
        out_u->set_shield(float(u.shields.to_double()));
        out_u->set_shield_max(float(u.shields_max.to_double()));
        out_u->set_energy(float(u.energy.to_double()));
        out_u->set_energy_max(float(u.energy_max.to_double()));
        out_u->set_is_flying(u.flying);
        out_u->set_is_burrowed(u.burrowed);
        if (alliance_of(u.player, joined_sim_side) == SC2APIProtocol::Self)
            fill_unit_orders(*game, u, abilities, *out_u);

        if (u.resource_left > 0) {
            if (u.is_gas)
                out_u->set_vespene_contents(u.resource_left);
            else
                out_u->set_mineral_contents(u.resource_left);
        }
    }
}

}
