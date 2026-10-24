#include "sc2/app/shipping_wiring.hpp"

#include <cstdlib>
#include <exception>
#include <string>
#include <utility>

#include "sc2/app/ability.hpp"
#include "sc2/app/melee_game.hpp"
#include "sc2/app/melee_research.hpp"
#include "sc2/data/catalog.hpp"
#include "sc2/data/production.hpp"
#include "sc2/data/requirements.hpp"
#include "sc2/render/offscreen.hpp"

#include <algorithm>
#include <map>
#include <set>

namespace sc2::app {

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
    game.world().set_accelerating_movers(std::getenv("SC2_MOVERS_CONSTANT") == nullptr);
    game.world().set_turret_aim(std::getenv("SC2_TURRET_AIM_BODY") == nullptr);
    game.world().set_exact_build_progress(std::getenv("SC2_BUILD_PROGRESS_TRUNCATED") == nullptr);
    game.world().set_gas_worker_inside(std::getenv("SC2_GAS_WORKER_OUTSIDE") == nullptr);
    game.world().set_gas_harvest_ordered(std::getenv("SC2_GAS_HARVEST_ANY") == nullptr);
    game.world().set_cargo_hidden(std::getenv("SC2_CARGO_SHOWN") == nullptr);
    game.world().set_cliff_leap(std::getenv("SC2_REAPER_ONE_CELL_LEAP") == nullptr);
    game.world().set_morph_vital_fraction(std::getenv("SC2_MORPH_HP_ABSOLUTE") == nullptr);
    game.world().set_path_nearest_fallback(std::getenv("SC2_PATH_STRAIGHT_FALLBACK") == nullptr);
    game.world().set_smart_structure_contact(std::getenv("SC2_SMART_STRUCTURE_TO_CENTRE") == nullptr);
    game.world().set_research_queue(std::getenv("SC2_RESEARCH_CONCURRENT") == nullptr);
    game.world().set_research_dies_with_host(std::getenv("SC2_RESEARCH_OUTLIVES_HOST") == nullptr);
    game.world().set_presentation_log(std::getenv("SC2_NO_EFFECT_ACTORS") == nullptr);
    game.world().set_gather_dock_footprint(std::getenv("SC2_GATHER_DOCK_RADIUS") == nullptr);
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
    game.set_selection_warm(std::getenv("SC2_NO_SELECTION_WARM") == nullptr);
    {
        const char* hz = std::getenv("SC2_PORTRAIT_HZ");
        game.set_portrait_hz(hz ? float(std::atof(hz)) : 30.0f);
    }
    game.set_pick_broadphase(std::getenv("SC2_PICK_BROADPHASE_OLD") == nullptr);
    game.set_select_rules_672(std::getenv("SC2_SELECT_MODES_OLD") == nullptr);
    game.set_portrait_async(std::getenv("SC2_PORTRAIT_DECODE_SYNC") == nullptr);
    game.set_warm_foreign(std::getenv("SC2_NO_WARM_FOREIGN") == nullptr);
    game.set_card_cache(std::getenv("SC2_CARD_COMPOSE_EVERY_FRAME") == nullptr);
    game.set_morph_transition_models(std::getenv("SC2_MORPH_NO_TRANSITION_MODEL") == nullptr);
    game.set_construction_child_team(std::getenv("SC2_CHILD_TEAM_TINT") != nullptr);
    render::world_async_loads() = std::getenv("SC2_WORLD_DECODE_SYNC") == nullptr;
    render::offscreen_on_window_device() = std::getenv("SC2_OFFSCREEN_OWN_DEVICE") == nullptr;
    render::texture_take_once() = std::getenv("SC2_TEX_TAKE_ONCE_OLD") == nullptr;
    if (const char* mb = std::getenv("SC2_UPLOAD_BATCH_MB")) render::upload_batch_budget_mb() = std::size_t(std::max(0, std::atoi(mb)));
    game.world().set_arc_movement(true);
}

}
