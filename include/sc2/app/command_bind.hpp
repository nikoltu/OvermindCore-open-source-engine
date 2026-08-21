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
    bool can_afford_supply = true;
    bool addon_producer_selected = false;
    bool can_afford_addon = true;
    bool can_afford_factory = true;
    bool cc_morph_selected = false;
    bool can_afford_orbital = true;
    bool can_afford_engbay = true;
    bool engbay_selected = false;
    bool can_afford_research = true;
    bool liberator_selected = false;
    bool liberator_ag_selected = false;
    bool templar_merge_selected = false;
    bool burrow_selected = false;
    bool unburrow_selected = false;
    bool liftoff_selected = false;
    bool land_selected = false;
    bool depot_lower_selected = false;
    bool depot_raise_selected = false;
    bool transport_unload_selected = false;
    bool worker_gas_selected = false;
    bool can_afford_refinery = true;
    std::string refinery_name;
    bool worker_expand_selected = false;
    bool can_afford_town_hall = true;
    std::string town_hall_name;
    bool worker_starport_selected = false;
    bool can_afford_starport = true;
    bool worker_pylon_selected = false;
    bool can_afford_pylon = true;
    bool worker_gateway_selected = false;
    bool can_afford_gateway = true;
};

enum MeleeCardAbility : std::uint32_t {
    kMeleeAbilNone = 0,
    kMeleeAbilStop = 4,
    kMeleeAbilHold = 5,
    kMeleeAbilAttack = 6,
    kMeleeAbilPatrol = 7,
    kMeleeAbilBuildTechLab = 9,
    kMeleeAbilBuildReactor = 10,
    kMeleeAbilMorphOrbital = 13,
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
};

inline CommandCard build_melee_command_card(const MeleeCommandContext& ctx) {
    CommandCard card;
    std::vector<CardEntry> entries;

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

    if (ctx.mobile_selected && !ctx.worker_selected) {
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
