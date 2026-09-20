#include "sc2/app/shipping_wiring.hpp"

#include <exception>
#include <string>
#include <utility>

#include "sc2/app/ability.hpp"
#include "sc2/app/melee_game.hpp"
#include "sc2/app/melee_research.hpp"
#include "sc2/data/catalog.hpp"
#include "sc2/data/production.hpp"
#include "sc2/data/requirements.hpp"

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
    game.world().set_arc_movement(true);
}

}
