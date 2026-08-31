#include "sc2/api/server.hpp"

#include <cstdint>
#include <string>
#include <vector>

#include "s2clientprotocol/sc2api.pb.h"

#include "sc2/api/bridge.hpp"
#include "sc2/api/orders.hpp"
#include "sc2/api/truth_pack.hpp"
#include "sc2/app/melee_game.hpp"

namespace sc2::api {

namespace {

app::Race race_from(SC2APIProtocol::Race r) {
    switch (r) {
        case SC2APIProtocol::Protoss: return app::Race::Protoss;
        case SC2APIProtocol::Zerg: return app::Race::Zerg;
        default: return app::Race::Terran;
    }
}

SC2APIProtocol::ActionResult apply_action(app::MeleeGame* game, int joined_sim_side,
                                          const AbilityPack& abilities,
                                          const SC2APIProtocol::Action& a) {
    if (!a.has_action_raw()) return SC2APIProtocol::NotSupported;
    const SC2APIProtocol::ActionRaw& raw = a.action_raw();
    if (raw.action_case() != SC2APIProtocol::ActionRaw::kUnitCommand)
        return SC2APIProtocol::NotSupported;
    const SC2APIProtocol::ActionRawUnitCommand& c = raw.unit_command();
    if (!game) return SC2APIProtocol::Error;
    if (!c.has_ability_id() || c.unit_tags().empty()) return SC2APIProtocol::Error;
    if (!abilities.loaded) return SC2APIProtocol::NotSupported;

    sim::World& world = game->world();

    for (const std::uint64_t tag : c.unit_tags()) {
        if (tag >= world.units().size()) return SC2APIProtocol::Error;
        const sim::Unit& u = world.unit(std::uint32_t(tag));
        if (!u.alive) return SC2APIProtocol::Error;
        if (u.player != joined_sim_side) return SC2APIProtocol::YouCantControlThatUnit;
    }

    const std::uint32_t ability = std::uint32_t(c.ability_id());

    const bool has_point = c.has_target_world_space_pos();
    const bool has_tag = c.has_target_unit_tag();

    if (is_stop_ability(abilities, ability)) {
        if (has_point || has_tag) return SC2APIProtocol::Error;
        for (const std::uint64_t tag : c.unit_tags()) world.stop(std::uint32_t(tag));
        return SC2APIProtocol::Success;
    }

    sim::OrderType type = sim::OrderType::Idle;
    if (!order_type_of_ability(abilities, ability, has_point, type))
        return SC2APIProtocol::NotSupported;

    const TargetForm want = required_target_form(type);
    switch (want) {
        case TargetForm::Point:
            if (has_tag) return SC2APIProtocol::NotSupported;
            if (!has_point) return SC2APIProtocol::Error;
            break;
        case TargetForm::UnitTag:
            if (!has_tag) return SC2APIProtocol::MustTargetUnit;
            break;
        case TargetForm::None:
            if (has_point || has_tag) return SC2APIProtocol::Error;
            break;
    }

    sim::Order order;
    order.type = type;
    if (has_tag) {
        const std::uint64_t target = c.target_unit_tag();
        if (target >= world.units().size() || !world.unit(std::uint32_t(target)).alive)
            return SC2APIProtocol::CantTargetThatUnit;
        order.target = std::uint32_t(target);
    } else if (has_point) {
        order.tx = sim::Fixed::from_double(double(c.target_world_space_pos().x()));
        order.ty = sim::Fixed::from_double(double(c.target_world_space_pos().y()));
    }

    for (const std::uint64_t tag : c.unit_tags()) {
        sim::Order per_unit = order;
        if (type == sim::OrderType::Patrol) {
            const sim::Unit& u = world.unit(std::uint32_t(tag));
            per_unit.px = u.x;
            per_unit.py = u.y;
        }
        world.issue(std::uint32_t(tag), per_unit, c.queue_command());
    }
    return SC2APIProtocol::Success;
}

}

void Dispatcher::reset() {
    status_ = SC2APIProtocol::launched;
    view_player_ = 1;
    view_revealed_ = false;
    realtime_ = false;
}

bool Dispatcher::handle(Host& host, const std::vector<std::uint8_t>& raw,
                        std::vector<std::uint8_t>& out_bytes) {
    if (status_ == 0) status_ = SC2APIProtocol::launched;
    bridge::View view;
    view.player = view_player_;
    view.revealed = view_revealed_;

    SC2APIProtocol::Request req;
    SC2APIProtocol::Response resp;
    const bool parsed = req.ParseFromArray(raw.data(), int(raw.size()));
    if (!parsed) {
        resp.add_error("the request did not parse as an SC2APIProtocol.Request");
        resp.set_status(SC2APIProtocol::Status(status_));
    } else {
        if (req.has_id()) resp.set_id(req.id());

        switch (req.request_case()) {
            case SC2APIProtocol::Request::kPing: {
                SC2APIProtocol::ResponsePing* p = resp.mutable_ping();
                p->set_game_version(host.game_version());
                p->set_data_build(host.data_build());
                p->set_base_build(host.data_build());
                break;
            }
            case SC2APIProtocol::Request::kCreateGame: {
                const SC2APIProtocol::RequestCreateGame& cg = req.create_game();
                SC2APIProtocol::ResponseCreateGame* out = resp.mutable_create_game();
                std::string err;
                if (!cg.has_local_map()) {
                    out->set_error(SC2APIProtocol::ResponseCreateGame::MissingMap);
                    out->set_error_details(
                        "this client serves local_map only. online service-served maps are an "
                        "owner-locked honest limit (design section 2.2); send map_data or "
                        "map_path.");
                    break;
                }
                std::vector<std::uint8_t> bytes;
                if (cg.local_map().has_map_data()) {
                    const std::string& d = cg.local_map().map_data();
                    bytes.assign(d.begin(), d.end());
                } else if (cg.local_map().has_map_path()) {
                    if (!host.read_map_file(cg.local_map().map_path(), bytes, err)) {
                        out->set_error(SC2APIProtocol::ResponseCreateGame::InvalidMapPath);
                        out->set_error_details(err);
                        break;
                    }
                } else {
                    out->set_error(SC2APIProtocol::ResponseCreateGame::MissingMap);
                    out->set_error_details("local_map carried neither map_data nor map_path");
                    break;
                }
                app::Race p0 = app::Race::Terran, p1 = app::Race::Terran;
                bool refused = false;
                int slot = 0;
                for (const SC2APIProtocol::PlayerSetup& ps : cg.player_setup()) {
                    if (ps.type() == SC2APIProtocol::Observer) {
                        out->set_error(SC2APIProtocol::ResponseCreateGame::InvalidPlayerSetup);
                        out->set_error_details(
                            "an Observer slot is out of scope for this client: it would need a "
                            "second, non-participant view of the match, which nothing in the "
                            "engine has. Use a Participant slot.");
                        refused = true;
                        break;
                    }
                    if (slot == 0) p0 = race_from(ps.race());
                    else if (slot == 1) p1 = race_from(ps.race());
                    ++slot;
                }
                if (refused) break;
                if (cg.has_random_seed()) {
                    out->set_error(SC2APIProtocol::ResponseCreateGame::MultiplayerUnsupported);
                    out->set_error_details(
                        "an externally supplied random_seed is not honoured by this client: "
                        "nothing in FrontEnd::begin_match takes one. It is refused rather than "
                        "ignored, so a run that needed it cannot read as a success.");
                    break;
                }
                if (!host.create_match(std::move(bytes), p0, p1, err)) {
                    out->set_error(SC2APIProtocol::ResponseCreateGame::InvalidMapData);
                    out->set_error_details(err);
                    break;
                }
                realtime_ = cg.realtime();
                host.set_realtime(realtime_);
                view = bridge::View{};
                status_ = SC2APIProtocol::init_game;
                break;
            }
            case SC2APIProtocol::Request::kJoinGame: {
                const SC2APIProtocol::RequestJoinGame& jg = req.join_game();
                SC2APIProtocol::ResponseJoinGame* out = resp.mutable_join_game();
                if (jg.has_server_ports() || !jg.client_ports().empty()) {
                    out->set_error(SC2APIProtocol::ResponseJoinGame::FeatureUnsupported);
                    out->set_error_details(
                        "multiplayer port sets are out of scope for this client (design section "
                        "2.2). They are refused rather than ignored, so a two-client run cannot "
                        "look supported.");
                    break;
                }
                if (jg.has_observed_player_id()) {
                    out->set_error(SC2APIProtocol::ResponseJoinGame::MissingParticipation);
                    out->set_error_details(
                        "joining as an observer is out of scope for this client (design section "
                        "2.2)");
                    break;
                }
                if (jg.has_options() && jg.options().has_render()) {
                    out->set_error(SC2APIProtocol::ResponseJoinGame::FeatureUnsupported);
                    out->set_error_details(
                        "the render interface arrives at Stage 6; asking for it now would get a "
                        "plausible empty answer, so it is refused instead.");
                    break;
                }
                if (jg.has_options() && jg.options().has_feature_layer()) {
                    out->set_error(SC2APIProtocol::ResponseJoinGame::FeatureUnsupported);
                    out->set_error_details(
                        "the feature layer is deliberately out of scope (design section 2.2): it "
                        "is a second rasterisation of data raw already carries.");
                    break;
                }
                out->set_player_id(std::uint32_t(view.player));
                status_ = SC2APIProtocol::in_game;
                break;
            }
            case SC2APIProtocol::Request::kGameInfo: {
                bridge::fill_game_info(host, view, *resp.mutable_game_info());
                break;
            }
            case SC2APIProtocol::Request::kObservation: {
                bridge::View v = view;
                v.disable_fog = v.disable_fog || req.observation().disable_fog();
                bridge::fill_observation(host, v, *resp.mutable_observation());
                break;
            }
            case SC2APIProtocol::Request::kStep: {
                if (realtime_) {
                    resp.add_error(
                        "Step is not available in realtime mode - the wall clock is driving the "
                        "sim. This matches the retail client's own behaviour.");
                    resp.mutable_step();
                    break;
                }
                const int loop = host.step(int(req.step().count()));
                resp.mutable_step()->set_simulation_loop(std::uint32_t(loop));
                break;
            }
            case SC2APIProtocol::Request::kData: {
                const SC2APIProtocol::RequestData& rd = req.data();
                SC2APIProtocol::ResponseData* out = resp.mutable_data();
                if (rd.ability_id() || rd.upgrade_id() || rd.buff_id() || rd.effect_id()) {
                    resp.add_error(
                        "only Data(unit_type_id) is served at Stage 1. The other id spaces arrive "
                        "with the requests that need them; answering them with an empty list now "
                        "would read as agreement.");
                    break;
                }
                std::string err;
                if (rd.unit_type_id() && !bridge::fill_unit_type_data(host, *out, err))
                    resp.add_error(err);
                break;
            }
            case SC2APIProtocol::Request::kDebug: {
                bool served = true;
                for (const SC2APIProtocol::DebugCommand& c : req.debug().debug()) {
                    if (c.command_case() == SC2APIProtocol::DebugCommand::kGameState &&
                        c.game_state() == SC2APIProtocol::show_map) {
                        view.revealed = true;
                    } else {
                        served = false;
                    }
                }
                if (!served)
                    resp.add_error(
                        "Stage 1 serves Debug(game_state = show_map) only; create_unit, "
                        "unit_value and the rest arrive at Stage 4. An unserved debug command is "
                        "refused rather than silently dropped.");
                resp.mutable_debug();
                break;
            }
            case SC2APIProtocol::Request::kAction: {
                SC2APIProtocol::ResponseAction* out = resp.mutable_action();
                const AbilityPack abilities =
                    load_ability_pack(host.truth_data_dir(), host.data_build());
                if (!abilities.loaded && !req.action().actions().empty())
                    resp.add_error("actions are addressed by retail ability id and this client "
                                   "joins that id space through the frozen ability truth pack: " +
                                   abilities.error);
                for (const SC2APIProtocol::Action& a : req.action().actions())
                    out->add_result(apply_action(host.match(), view.player - 1, abilities, a));
                break;
            }
            case SC2APIProtocol::Request::kStartReplay: {
                SC2APIProtocol::ResponseStartReplay* out = resp.mutable_start_replay();
                out->set_error(SC2APIProtocol::ResponseStartReplay::LaunchError);
                out->set_error_details(
                    "this client does not play .SC2Replay. Its replay format is the engine's own "
                    "order log, and .SC2Replay byte-format parity is a declared honest limit, not "
                    "pending work - so this request is refused permanently rather than left to "
                    "look unimplemented. To compare a retail replay against this engine, run the "
                    "replay in the retail client and drive this one over the same map with "
                    "RequestStep and RequestObservation.");
                break;
            }
            case SC2APIProtocol::Request::kLeaveGame: {
                host.leave_match();
                resp.mutable_leave_game();
                status_ = SC2APIProtocol::launched;
                view = bridge::View{};
                break;
            }
            case SC2APIProtocol::Request::kQuit: {
                resp.mutable_quit();
                break;
            }
            default:
                resp.add_error(
                    "that request is not served by this client. It is refused explicitly rather "
                    "than answered with a plausible empty value, so a report that depended on it "
                    "reads INCOMPARABLE instead of a false agreement (design section 5.4).");
                break;
        }
        resp.set_status(SC2APIProtocol::Status(status_));
    }

    view_player_ = view.player;
    view_revealed_ = view.revealed;

    out_bytes.assign(resp.ByteSizeLong(), 0);
    (void)resp.SerializeToArray(out_bytes.data(), int(out_bytes.size()));
    return parsed;
}

}
