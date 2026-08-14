#pragma once

#include <string>
#include <vector>

#include "sc2/app/command_card.hpp"

namespace sc2::app {

struct MeleeCommandContext {
    bool worker_selected = false;
    bool town_hall_selected = false;
    bool producer_built = false;
    bool combat_selected = false;
    bool mobile_selected = false;
    bool can_afford_producer = true;
    bool can_afford_army = true;
    bool can_afford_worker = true;
    bool can_afford_supply = true;
    bool addon_producer_selected = false;
    bool can_afford_addon = true;
    bool techlab_producer_selected = false;
    bool can_afford_marauder = true;
    std::string techlab_unit_name;
    bool can_afford_factory = true;
    bool cc_morph_selected = false;
    bool can_afford_orbital = true;
    bool orbital_selected = false;
    bool orbital_has_energy = false;
    bool can_afford_engbay = true;
    bool engbay_selected = false;
    bool can_afford_research = true;
    bool liberator_selected = false;
    bool liberator_ag_selected = false;
    bool templar_merge_selected = false;
    bool burrow_selected = false;
    bool unburrow_selected = false;
    bool queen_selected = false;
    bool inject_ready = true;
    bool liftoff_selected = false;
    bool land_selected = false;
    bool depot_lower_selected = false;
    bool depot_raise_selected = false;
    bool transport_unload_selected = false;
    bool infestor_selected = false;
    bool infested_terran_ready = true;
    bool worker_gas_selected = false;
    bool can_afford_refinery = true;
    std::string refinery_name;
    bool worker_expand_selected = false;
    bool can_afford_town_hall = true;
    std::string town_hall_name;
    bool worker_starport_selected = false;
    bool can_afford_starport = true;
    bool starport_selected = false;
    bool can_afford_air = true;
    std::string air_unit_name;
    bool worker_pylon_selected = false;
    bool can_afford_pylon = true;
    bool worker_gateway_selected = false;
    bool can_afford_gateway = true;
    bool gateway_selected = false;
    bool can_afford_zealot = true;
};

enum MeleeCardAbility : std::uint32_t {
    kMeleeAbilNone = 0,
    kMeleeAbilBuildProducer = 1,
    kMeleeAbilTrainArmy = 2,
    kMeleeAbilTrainWorker = 3,
    kMeleeAbilStop = 4,
    kMeleeAbilHold = 5,
    kMeleeAbilAttack = 6,
    kMeleeAbilPatrol = 7,
    kMeleeAbilBuildSupply = 8,
    kMeleeAbilBuildTechLab = 9,
    kMeleeAbilBuildReactor = 10,
    kMeleeAbilTrainTechUnit = 11,
    kMeleeAbilBuildFactory = 12,
    kMeleeAbilMorphOrbital = 13,
    kMeleeAbilCalldownMule = 15,
    kMeleeAbilBuildEngBay = 16,
    kMeleeAbilResearchInfWeapons = 17,
    kMeleeAbilLiberatorDefender = 18,
    kMeleeAbilLiberatorFighter = 19,
    kMeleeAbilMergeArchon = 20,
    kMeleeAbilBurrow = 22,
    kMeleeAbilUnburrow = 23,
    kMeleeAbilLiftOff = 26,
    kMeleeAbilLand = 27,
    kMeleeAbilLowerDepot = 28,
    kMeleeAbilRaiseDepot = 29,
    kMeleeAbilUnloadAll = 30,
    kMeleeAbilSpawnLarva = 32,
    kMeleeAbilSpawnInfestedTerran = 36,
    kMeleeAbilBuildRefinery = 37,
    kMeleeAbilBuildTownHall = 38,
    kMeleeAbilBuildStarport = 39,
    kMeleeAbilTrainAir = 40,
    kMeleeAbilBuildPylon = 41,
    kMeleeAbilBuildGateway = 42,
    kMeleeAbilTrainZealot = 43,
};

inline CommandCard build_melee_command_card(const MeleeCommandContext& ctx) {
    CommandCard card;
    std::vector<CardEntry> entries;

    if (ctx.town_hall_selected) {
        CardEntry e;
        e.slot = 0;
        e.icon = "btn-unit-terran-scv";
        e.name = "Train SCV";
        e.hotkey = "S";
        e.ability = kMeleeAbilTrainWorker;
        e.requirement_met = ctx.can_afford_worker;
        entries.push_back(e);
    }
    if (ctx.worker_selected && !ctx.town_hall_selected && !ctx.producer_built) {
        CardEntry e;
        e.slot = 0;
        e.icon = "btn-building-terran-barracks";
        e.name = "Build Barracks";
        e.hotkey = "B";
        e.ability = kMeleeAbilBuildProducer;
        e.requirement_met = ctx.can_afford_producer;
        entries.push_back(e);
    }
    if (ctx.worker_selected && !ctx.town_hall_selected) {
        CardEntry e;
        e.slot = 2;
        e.icon = "btn-building-terran-supplydepot";
        e.name = "Build Supply Depot";
        e.hotkey = "D";
        e.ability = kMeleeAbilBuildSupply;
        e.requirement_met = ctx.can_afford_supply;
        entries.push_back(e);
    }
    if (ctx.worker_selected && !ctx.town_hall_selected) {
        CardEntry e;
        e.slot = 4;
        e.icon = "btn-building-terran-engineeringbay";
        e.name = "Build Engineering Bay";
        e.hotkey = "E";
        e.ability = kMeleeAbilBuildEngBay;
        e.requirement_met = ctx.can_afford_engbay;
        entries.push_back(e);
    }
    if (ctx.worker_gas_selected) {
        CardEntry e;
        e.slot = 6;
        e.icon = "btn-building-terran-refinery";
        e.name = ctx.refinery_name.empty() ? std::string("Build Refinery") : ("Build " + ctx.refinery_name);
        e.hotkey = "G";
        e.ability = kMeleeAbilBuildRefinery;
        e.requirement_met = ctx.can_afford_refinery;
        entries.push_back(e);
    }
    if (ctx.worker_expand_selected) {
        CardEntry e;
        e.slot = 7;
        e.icon = "btn-building-terran-commandcenter";
        e.name = ctx.town_hall_name.empty() ? std::string("Build Command Center")
                                            : ("Build " + ctx.town_hall_name);
        e.hotkey = "C";
        e.ability = kMeleeAbilBuildTownHall;
        e.requirement_met = ctx.can_afford_town_hall;
        entries.push_back(e);
    }
    if (ctx.worker_selected && !ctx.town_hall_selected && ctx.producer_built) {
        CardEntry e;
        e.slot = 3;
        e.icon = "btn-building-terran-factory";
        e.name = "Build Factory";
        e.hotkey = "F";
        e.ability = kMeleeAbilBuildFactory;
        e.requirement_met = ctx.can_afford_factory;
        entries.push_back(e);
    }
    if (ctx.worker_starport_selected) {
        CardEntry e;
        e.slot = 8;
        e.icon = "btn-building-terran-starport";
        e.name = "Build Starport";
        e.hotkey = "P";
        e.ability = kMeleeAbilBuildStarport;
        e.requirement_met = ctx.can_afford_starport;
        entries.push_back(e);
    }
    if (ctx.worker_pylon_selected) {
        CardEntry e;
        e.slot = 5;
        e.icon = "btn-building-protoss-pylon";
        e.name = "Build Pylon";
        e.hotkey = "E";
        e.ability = kMeleeAbilBuildPylon;
        e.requirement_met = ctx.can_afford_pylon;
        entries.push_back(e);
    }
    if (ctx.worker_gateway_selected) {
        CardEntry e;
        e.slot = 9;
        e.icon = "btn-building-protoss-gateway";
        e.name = "Build Gateway";
        e.hotkey = "G";
        e.ability = kMeleeAbilBuildGateway;
        e.requirement_met = ctx.can_afford_gateway;
        entries.push_back(e);
    }
    if (ctx.producer_built) {
        CardEntry e;
        e.slot = 1;
        e.icon = "btn-unit-terran-marine";
        e.name = "Train Marine";
        e.hotkey = "T";
        e.ability = kMeleeAbilTrainArmy;
        e.requirement_met = ctx.can_afford_army;
        entries.push_back(e);
    }
    if (ctx.addon_producer_selected) {
        CardEntry tl;
        tl.slot = 3;
        tl.icon = "btn-building-terran-techlab";
        tl.name = "Build Tech Lab";
        tl.hotkey = "C";
        tl.ability = kMeleeAbilBuildTechLab;
        tl.requirement_met = ctx.can_afford_addon;
        entries.push_back(tl);
        CardEntry rc;
        rc.slot = 4;
        rc.icon = "btn-building-terran-reactor";
        rc.name = "Build Reactor";
        rc.hotkey = "R";
        rc.ability = kMeleeAbilBuildReactor;
        rc.requirement_met = ctx.can_afford_addon;
        entries.push_back(rc);
    }
    if (ctx.techlab_producer_selected && !ctx.techlab_unit_name.empty()) {
        CardEntry e;
        e.slot = 5;
        e.icon = "btn-unit-terran-marauder";
        e.name = "Train " + ctx.techlab_unit_name;
        e.hotkey = "D";
        e.ability = kMeleeAbilTrainTechUnit;
        e.requirement_met = ctx.can_afford_marauder;
        entries.push_back(e);
    }
    if (ctx.starport_selected && !ctx.air_unit_name.empty()) {
        CardEntry e;
        e.slot = 0;
        e.icon = "btn-unit-terran-viking";
        e.name = "Train " + ctx.air_unit_name;
        e.hotkey = "V";
        e.ability = kMeleeAbilTrainAir;
        e.requirement_met = ctx.can_afford_air;
        entries.push_back(e);
    }
    if (ctx.gateway_selected) {
        CardEntry e;
        e.slot = 0;
        e.icon = "btn-unit-protoss-zealot";
        e.name = "Train Zealot";
        e.hotkey = "Z";
        e.ability = kMeleeAbilTrainZealot;
        e.requirement_met = ctx.can_afford_zealot;
        entries.push_back(e);
    }

    if (ctx.cc_morph_selected) {
        CardEntry e;
        e.slot = 3;
        e.icon = "btn-building-terran-orbitalcommand";
        e.name = "Morph to Orbital Command";
        e.hotkey = "V";
        e.ability = kMeleeAbilMorphOrbital;
        e.requirement_met = ctx.can_afford_orbital;
        entries.push_back(e);
    }
    if (ctx.orbital_selected) {
        CardEntry mu;
        mu.slot = 5;
        mu.icon = "btn-ability-terran-calldownmule";
        mu.name = "Calldown MULE";
        mu.hotkey = "E";
        mu.ability = kMeleeAbilCalldownMule;
        mu.requirement_met = ctx.orbital_has_energy;
        entries.push_back(mu);
    }
    if (ctx.engbay_selected) {
        CardEntry e;
        e.slot = 0;
        e.icon = "btn-upgrade-terran-infantryweaponslevel0";
        e.name = "Research Infantry Weapons";
        e.hotkey = "A";
        e.ability = kMeleeAbilResearchInfWeapons;
        e.requirement_met = ctx.can_afford_research;
        entries.push_back(e);
    }
    if (ctx.liberator_selected) {
        CardEntry e;
        e.slot = 0;
        e.icon = "btn-ability-terran-liberatoragmode";
        e.name = "Defender Mode";
        e.hotkey = "D";
        e.ability = kMeleeAbilLiberatorDefender;
        e.requirement_met = true;
        entries.push_back(e);
    }
    if (ctx.liberator_ag_selected) {
        CardEntry e;
        e.slot = 0;
        e.icon = "btn-ability-terran-liberatoraamode";
        e.name = "Fighter Mode";
        e.hotkey = "D";
        e.ability = kMeleeAbilLiberatorFighter;
        e.requirement_met = true;
        entries.push_back(e);
    }

    if (ctx.templar_merge_selected) {
        CardEntry e;
        e.slot = 0;
        e.icon = "btn-ability-protoss-archonwarp";
        e.name = "Merge into Archon";
        e.hotkey = "R";
        e.ability = kMeleeAbilMergeArchon;
        e.requirement_met = true;
        entries.push_back(e);
    }

    if (ctx.queen_selected) {
        CardEntry e;
        e.slot = 1;
        e.icon = "btn-ability-zerg-spawnlarva";
        e.name = "Spawn Larva";
        e.hotkey = "V";
        e.ability = kMeleeAbilSpawnLarva;
        e.requirement_met = ctx.inject_ready;
        entries.push_back(e);
    }

    if (ctx.infestor_selected) {
        CardEntry e;
        e.slot = 0;
        e.icon = "btn-ability-zerg-infestedterrans";
        e.name = "Spawn Infested Terran";
        e.hotkey = "T";
        e.ability = kMeleeAbilSpawnInfestedTerran;
        e.requirement_met = ctx.infested_terran_ready;
        entries.push_back(e);
    }

    if (ctx.burrow_selected) {
        CardEntry e;
        e.slot = 0;
        e.icon = "btn-ability-zerg-burrow";
        e.name = "Burrow";
        e.hotkey = "R";
        e.ability = kMeleeAbilBurrow;
        e.requirement_met = true;
        entries.push_back(e);
    }
    if (ctx.unburrow_selected) {
        CardEntry e;
        e.slot = 1;
        e.icon = "btn-ability-zerg-burrowup";
        e.name = "Unburrow";
        e.hotkey = "R";
        e.ability = kMeleeAbilUnburrow;
        e.requirement_met = true;
        entries.push_back(e);
    }

    if (ctx.liftoff_selected) {
        CardEntry e;
        e.slot = 2;
        e.icon = "btn-ability-terran-liftoff";
        e.name = "Lift Off";
        e.hotkey = "L";
        e.ability = kMeleeAbilLiftOff;
        e.requirement_met = true;
        entries.push_back(e);
    }
    if (ctx.land_selected) {
        CardEntry e;
        e.slot = 2;
        e.icon = "btn-ability-terran-land";
        e.name = "Land";
        e.hotkey = "L";
        e.ability = kMeleeAbilLand;
        e.requirement_met = true;
        entries.push_back(e);
    }

    if (ctx.depot_lower_selected) {
        CardEntry e;
        e.slot = 2;
        e.icon = "btn-ability-terran-supplydepotlower";
        e.name = "Lower Supply Depot";
        e.hotkey = "R";
        e.ability = kMeleeAbilLowerDepot;
        e.requirement_met = true;
        entries.push_back(e);
    }
    if (ctx.depot_raise_selected) {
        CardEntry e;
        e.slot = 2;
        e.icon = "btn-ability-terran-supplydepotraise";
        e.name = "Raise Supply Depot";
        e.hotkey = "R";
        e.ability = kMeleeAbilRaiseDepot;
        e.requirement_met = true;
        entries.push_back(e);
    }

    if (ctx.transport_unload_selected) {
        CardEntry e;
        e.slot = 0;
        e.icon = "btn-command-unloadall";
        e.name = "Unload All";
        e.hotkey = "D";
        e.ability = kMeleeAbilUnloadAll;
        e.requirement_met = true;
        entries.push_back(e);
    }

    if (ctx.mobile_selected) {
        struct Univ { int slot; const char* icon; const char* name; const char* hotkey; std::uint32_t ability; };
        static const Univ kUniv[] = {
            {10, "btn-command-stop", "Stop", "X", kMeleeAbilStop},
            {11, "btn-command-holdposition", "Hold Position", "H", kMeleeAbilHold},
            {12, "btn-command-attack", "Attack", "A", kMeleeAbilAttack},
            {13, "btn-command-patrol", "Patrol", "P", kMeleeAbilPatrol},
        };
        for (const Univ& u : kUniv) {
            CardEntry e;
            e.slot = u.slot;
            e.icon = u.icon;
            e.name = u.name;
            e.hotkey = u.hotkey;
            e.ability = u.ability;
            e.requirement_met = true;
            entries.push_back(e);
        }
    }

    card.fill(entries);
    if (!entries.empty()) card.set_cancel();
    return card;
}

}
