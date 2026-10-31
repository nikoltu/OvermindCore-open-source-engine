#include "omc/app/shipping_wiring.hpp"

#include <cstdlib>
#include <exception>
#include <string>
#include <utility>

#include "omc/ai/micro.hpp"
#include "omc/app/ability.hpp"
#include "omc/app/melee_game.hpp"
#include "omc/app/melee_research.hpp"
#include "omc/data/catalog.hpp"
#include "omc/data/production.hpp"
#include "omc/data/requirements.hpp"
#include "omc/render/offscreen.hpp"

#include <algorithm>
#include <map>
#include <set>

namespace omc::app {

namespace {

std::string race_tech_building(Race r) {
    switch (r) {
        case Race::Protoss: return "Forge";
        case Race::Zerg:    return "EvolutionChamber";
        case Race::Terran:
        default:            return "EngineeringBay";
    }
}

Race race_of_hall(const data::Catalog& cat, const std::string& hall) {
    const int r = cat.unit_race_index(hall);
    return r == 1 ? Race::Protoss : r == 2 ? Race::Zerg : Race::Terran;
}

}

std::vector<std::string> techlab_gated_units(const data::Catalog& cat,
                                             const data::Production& prod) {
    std::vector<std::string> techlabs, reactors;
    for (const std::string& uid : cat.ids("CUnit"))
        for (const std::string& al : cat.array_links("CUnit", uid, "TechAliasArray")) {
            if (al == "Alias_TechLab") techlabs.push_back(uid);
            if (al == "Alias_Reactor") reactors.push_back(uid);
        }
    if (techlabs.empty() || reactors.empty()) return {};

    data::TechState base;
    for (const std::string& rid : cat.ids("CRequirement")) {
        const data::Catalog::Entry* e = cat.find("CRequirement", rid);
        if (!e || e->tag != "CRequirementCountUnit") continue;
        for (const core::XmlNode& n : cat.resolved("CRequirement", rid))
            if (n.name == "Count") {
                const std::string link = n.attr_or("Link", "");
                if (!link.empty()) base.units_complete[link] = 1;
            }
    }
    data::TechState with_lab = base, with_reactor = base;
    for (const std::string& u : techlabs) with_lab.at_unit[u] = 1;
    for (const std::string& u : reactors) with_reactor.at_unit[u] = 1;

    std::map<std::string, bool> gated_req;
    auto is_techlab_gate = [&](const std::string& req) {
        auto it = gated_req.find(req);
        if (it != gated_req.end()) return it->second;
        const bool none = data::eval_requirement(cat, req, "Use", base);
        const bool lab = data::eval_requirement(cat, req, "Use", with_lab);
        const bool rct = data::eval_requirement(cat, req, "Use", with_reactor);
        return gated_req.emplace(req, !none && lab && !rct).first->second;
    };

    std::set<std::string> out;
    for (const std::string& uid : cat.ids("CUnit"))
        for (const data::ProductInfo& pi : prod.producers_of(uid))
            if (!pi.requirement.empty() && is_techlab_gate(pi.requirement)) {
                out.insert(uid);
                break;
            }
    return std::vector<std::string>(out.begin(), out.end());
}

void apply_shipping_wiring(MeleeGame& game, const data::Catalog& cat,
                           const data::UpgradeCatalog* up, const MeleeSetup* setup,
                           const std::vector<Race>* races) {

    game.bind_catalog(cat);

    {
        data::Production tech_prod;
        tech_prod.build(cat);
        for (const std::string& uid : techlab_gated_units(cat, tech_prod))
            game.world().require_attached_techlab(uid);
    }
    game.bind_actor_rules(cat);
    game.enable_larva(cat);
    game.enable_collapsible_debris(cat);
    game.enable_broodlings(cat);
    game.enable_interceptors(cat);
    game.enable_novas(cat);
    game.enable_ai_casting_from_roster(cat);
    game.enable_ai_tech_gate(cat);
    game.set_train_gate_strict(std::getenv("OMC_TRAIN_GATE_OLD") == nullptr);
    game.set_ai_stock_gas(std::getenv("OMC_AI_STOCK_SKIP_GAS") == nullptr);
    game.set_ai_macro(std::getenv("OMC_AI_PENDING_CAP_OLD") == nullptr, std::getenv("OMC_AI_SUPPLY_MARGIN_OLD") == nullptr,
                      std::getenv("OMC_AI_TECH_DUP_OLD") == nullptr, std::getenv("OMC_AI_EXTRA_PRODUCER_OLD") == nullptr,
                      std::getenv("OMC_AI_ALT_WAIT_OLD") == nullptr, std::getenv("OMC_AI_GAS_IDLE_OLD") == nullptr);
    game.set_ai_close_out(std::getenv("OMC_AI_EXPAND_OLD") == nullptr, std::getenv("OMC_AI_LARVA_MACRO_OLD") == nullptr,
                          std::getenv("OMC_AI_HUNT_OLD") == nullptr, std::getenv("OMC_AI_EVICT_OLD") == nullptr,
                          std::getenv("OMC_CARGO_NO_DROPOFF_OLD") == nullptr);
    game.world().set_cargo_idle_without_dropoff(std::getenv("OMC_CARGO_NO_DROPOFF_OLD") == nullptr);
    game.set_hall_needs_builder(std::getenv("OMC_AI_HALL_BUILDER_OLD") == nullptr);
    game.set_hunt_gas(std::getenv("OMC_AI_HUNT_GAS_OLD") == nullptr);
    game.set_worker_transfer(std::getenv("OMC_AI_TRANSFER_OLD") == nullptr);
    game.set_ai_seal_guard(std::getenv("OMC_AI_SEAL_POCKET_OLD") == nullptr);
    game.world().set_stall_unreachable(std::getenv("OMC_MOVE_UNREACHABLE_OLD") == nullptr);
    console_race_assets() = std::getenv("OMC_CONSOLE_RACE_OLD") == nullptr;
    ai::focus_skips_neutral() = std::getenv("OMC_AI_FOCUS_NEUTRAL") == nullptr;
    game.world().set_separation_respects_nav(std::getenv("OMC_SEPARATION_IGNORES_NAV") == nullptr);
    game.world().set_pull_with_radius(std::getenv("OMC_PATH_PULL_CENTRE_LINE") == nullptr);
    game.world().set_acquire_skips_neutral(std::getenv("OMC_ACQUIRE_NEUTRAL") == nullptr);
    game.world().set_raise_evicts(std::getenv("OMC_DEPOT_RAISE_WAITS") == nullptr);
    game.world().set_gather_bounce(std::getenv("OMC_GATHER_WAIT_OLD") == nullptr);
    game.world().set_exit_open_spot(std::getenv("OMC_TRAIN_EXIT_OLD") == nullptr);
    game.world().set_spawn_open_spot(std::getenv("OMC_LARVA_SPAWN_OLD") == nullptr);
    game.world().set_return_dock_footprint(std::getenv("OMC_RETURN_DOCK_OLD") == nullptr);
    game.world().set_unstick_bodies(std::getenv("OMC_BODY_UNSTICK_OLD") == nullptr);
    game.world().set_disc_slide(std::getenv("OMC_DISC_SLIDE_OLD") == nullptr);
    game.world().set_rest_push_collide(std::getenv("OMC_REST_PUSH_COLLIDE_OLD") == nullptr);
    game.world().set_level_guard(std::getenv("OMC_MOVE_LEVEL_GUARD_OLD") == nullptr);
    game.world().set_sep_push_rules(std::getenv("OMC_SEP_PUSH_RULES_ON") != nullptr);

    if (up && setup) {
        std::vector<MeleeResearchDriver::ProtoNames> names(setup->players.size());
        std::vector<MeleeGame::AiResearchPlan> plans(setup->players.size());
        for (std::size_t p = 0; p < setup->players.size(); ++p) {
            const MeleePlayerSetup& ps = setup->players[p];
            names[p] = MeleeResearchDriver::ProtoNames{ps.army_unit.catalog_id, ps.worker.catalog_id,
                                                       ps.producer.catalog_id, ps.supply_unit.catalog_id};
            plans[p].upgrades = army_weapon_upgrade_ids(cat, *up, ps.army_unit.catalog_id);
            const std::vector<std::string> armor_ids =
                army_armor_upgrade_ids(*up, ps.army_unit.catalog_id);
            plans[p].upgrades.insert(plans[p].upgrades.end(), armor_ids.begin(), armor_ids.end());
            const std::vector<std::string> shield_ids =
                army_shield_upgrade_ids(*up, ps.army_unit.catalog_id);
            plans[p].upgrades.insert(plans[p].upgrades.end(), shield_ids.begin(), shield_ids.end());
            const Race r = (races && p < races->size()) ? (*races)[p]
                                                        : race_of_hall(cat, ps.town_hall.catalog_id);
            plans[p].tech_id = race_tech_building(r);
        }
        game.enable_research(cat, *up, std::move(names));
        game.enable_ai_research(std::move(plans));
    }

    game.world().set_weapon_executor(true);
    game.world().set_reserve_supply(true);
    game.world().set_train_affordability(true);
    game.world().set_attack_windup(true);
    game.world().set_projectile_travel(true);
    game.world().set_accelerating_movers(std::getenv("OMC_MOVERS_CONSTANT") == nullptr);
    game.world().set_turret_aim(std::getenv("OMC_TURRET_AIM_BODY") == nullptr);
    game.world().set_exact_build_progress(std::getenv("OMC_BUILD_PROGRESS_TRUNCATED") == nullptr);
    game.world().set_gas_worker_inside(std::getenv("OMC_GAS_WORKER_OUTSIDE") == nullptr);
    game.world().set_gas_harvest_ordered(std::getenv("OMC_GAS_HARVEST_ANY") == nullptr);
    game.world().set_cargo_hidden(std::getenv("OMC_CARGO_SHOWN") == nullptr);
    game.world().set_cliff_leap(std::getenv("OMC_REAPER_ONE_CELL_LEAP") == nullptr);
    game.world().set_morph_vital_fraction(std::getenv("OMC_MORPH_HP_ABSOLUTE") == nullptr);
    game.world().set_path_nearest_fallback(std::getenv("OMC_PATH_STRAIGHT_FALLBACK") == nullptr);
    game.world().set_smart_structure_contact(std::getenv("OMC_SMART_STRUCTURE_TO_CENTRE") == nullptr);
    game.world().set_smart_lowered_open(std::getenv("OMC_SMART_LOWERED_DEPOT_CONTACT") == nullptr);
    game.world().set_research_queue(std::getenv("OMC_RESEARCH_CONCURRENT") == nullptr);
    game.world().set_research_dies_with_host(std::getenv("OMC_RESEARCH_OUTLIVES_HOST") == nullptr);
    game.world().set_presentation_log(std::getenv("OMC_NO_EFFECT_ACTORS") == nullptr);
    game.world().set_gather_dock_footprint(std::getenv("OMC_GATHER_DOCK_RADIUS") == nullptr);
    game.world().set_death_reveal(true);
    auto cgame = [&](const char* field) -> sim::Fixed {
        const auto v = cat.tag_default_scalar("CGame", field);
        return (v && !v->empty()) ? sim::Fixed::parse(*v) : sim::Fixed();
    };
    if (cgame("AcquireLeashRadius").raw() > 0)
        game.world().set_leash(true, cgame("AcquireLeashRadius"), cgame("AcquireLeashResetRadius"));
    game.world().set_call_for_help(cgame("CallForHelpRadius"), cgame("CallForHelpPeriod"));
    game.world().set_attack_reveal(cgame("AttackRevealTime"));
    game.set_stalemate(cgame("StalemateTestTime"), cgame("StalemateWarningTime"));
    game.set_selection_warm(std::getenv("OMC_NO_SELECTION_WARM") == nullptr);
    {
        const char* hz = std::getenv("OMC_PORTRAIT_HZ");
        game.set_portrait_hz(hz ? float(std::atof(hz)) : 30.0f);
    }
    game.set_pick_broadphase(std::getenv("OMC_PICK_BROADPHASE_OLD") == nullptr);
    game.set_select_rules_672(std::getenv("OMC_SELECT_MODES_OLD") == nullptr);
    game.set_group_self_cast(std::getenv("OMC_GROUP_CAST_OLD") == nullptr);
    game.set_group_cast_cell(std::getenv("OMC_GROUP_CAST_CELL_OLD") == nullptr);
    game.set_best_unit_pick(std::getenv("OMC_BEST_UNIT_OLD") == nullptr);
    game.set_best_unit_target_pick(std::getenv("OMC_BEST_UNIT_TARGET_OLD") == nullptr);
    game.set_every_unit_target_cast(std::getenv("OMC_TARGET_CAST_EVERY_OLD") == nullptr);
    game.set_portrait_async(std::getenv("OMC_PORTRAIT_DECODE_SYNC") == nullptr);
    game.set_warm_foreign(std::getenv("OMC_NO_WARM_FOREIGN") == nullptr);
    game.set_card_cache(std::getenv("OMC_CARD_COMPOSE_EVERY_FRAME") == nullptr);
    game.set_morph_transition_models(std::getenv("OMC_MORPH_NO_TRANSITION_MODEL") == nullptr);
    game.set_morph_upgrade_fold(std::getenv("OMC_MORPH_UPGRADES_IGNORED") == nullptr);
    game.set_timed_burrow(std::getenv("OMC_BURROW_INSTANT") == nullptr);
    game.set_timed_depot(std::getenv("OMC_DEPOT_MORPH_TIMED") != nullptr);
    game.set_morph_lock_order(std::getenv("OMC_MORPH_LOCK_SIM_SECTIONS") == nullptr);
    game.set_morph_cocoon_models(std::getenv("OMC_MORPH_NO_COCOON_MODEL") == nullptr);
    game.set_morph_swap_at_data(std::getenv("OMC_MORPH_SWAP_AT_END") == nullptr);
    game.set_morph_behaviors_sim(std::getenv("OMC_MORPH_BEHAVIOR_SIM_OLD") == nullptr);
    game.set_upgrade_fold_new(std::getenv("OMC_UPGRADE_FOLD_LIVE_ONLY") == nullptr);
    game.set_upgrade_on_create(std::getenv("OMC_UPGRADE_ON_CREATE_OLD") == nullptr);
    game.set_abil_upgrade_fold(std::getenv("OMC_ABIL_UPGRADE_FOLD_OLD") == nullptr);
    game.set_defender_range(std::getenv("OMC_DEFENDER_RANGE_OLD") == nullptr);
    game.world().set_morph_keep_behaviors(std::getenv("OMC_MORPH_BEHAVIOR_KEEP_OLD") == nullptr);
    game.set_zone_markers(std::getenv("OMC_NO_ZONE_MARKER") == nullptr);
    game.set_actor_game_clock(std::getenv("OMC_ACTOR_WALL_CLOCK") == nullptr);
    game.set_construction_child_team(std::getenv("OMC_CHILD_TEAM_TINT") != nullptr);
    render::world_async_loads() = std::getenv("OMC_WORLD_DECODE_SYNC") == nullptr;
    render::offscreen_on_window_device() = std::getenv("OMC_OFFSCREEN_OWN_DEVICE") == nullptr;
    render::texture_take_once() = std::getenv("OMC_TEX_TAKE_ONCE_OLD") == nullptr;
    if (const char* mb = std::getenv("OMC_UPLOAD_BATCH_MB")) render::upload_batch_budget_mb() = std::size_t(std::max(0, std::atoi(mb)));
    game.world().set_arc_movement(true);
}

}
