#include "sc2/app/shipping_wiring.hpp"

#include <exception>
#include <string>
#include <utility>

#include "sc2/app/ability.hpp"
#include "sc2/app/melee_game.hpp"
#include "sc2/app/melee_research.hpp"
#include "sc2/data/catalog.hpp"
#include "sc2/data/production.hpp"

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

Race race_of_hall(const std::string& hall) {
    if (hall == "Nexus") return Race::Protoss;
    if (hall == "Hatchery" || hall == "Lair" || hall == "Hive") return Race::Zerg;
    return Race::Terran;
}

}

void apply_shipping_wiring(MeleeGame& game, const data::Catalog& cat,
                           const data::UpgradeCatalog* up, const MeleeSetup* setup,
                           const std::vector<Race>* races) {

    {
        data::Production tech_prod;
        tech_prod.build(cat);
        for (const std::string& uid : cat.ids("CUnit"))
            for (const data::ProductInfo& pi : tech_prod.producers_of(uid))
                if (pi.requirement.find("Attached") != std::string::npos &&
                    pi.requirement.find("TechLab") != std::string::npos) {
                    game.world().require_attached_techlab(uid);
                    break;
                }
    }
    game.enable_larva(cat);
    game.enable_collapsible_debris(cat);
    game.enable_broodlings(cat);
    game.enable_interceptors(cat);
    game.enable_novas(cat);
    game.enable_ai_casting_from_roster(cat);

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
                                                        : race_of_hall(ps.town_hall.catalog_id);
            plans[p].tech_id = race_tech_building(r);
        }
        game.enable_research(cat, *up, std::move(names));
        game.enable_ai_research(std::move(plans));
    }

    game.world().set_weapon_executor(true);
    game.world().set_reserve_supply(true);
    game.world().set_attack_windup(true);
    game.world().set_projectile_travel(true);
    game.world().set_death_reveal(true);
    game.world().set_arc_movement(true);
}

}
