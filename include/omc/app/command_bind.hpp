#pragma once

#include <string>
#include <vector>

#include "omc/app/command_card.hpp"

namespace omc::app {

struct MeleeCommandContext {
    std::string cancel_icon;

    struct UniversalCell {
        int slot = -1;
        std::string icon;
        std::string name;
        std::string hotkey;
    };
    UniversalCell univ_stop, univ_hold, univ_attack, univ_patrol;
    UniversalCell univ_burrow, univ_unburrow;
    UniversalCell univ_liftoff;
    UniversalCell univ_move;
    UniversalCell univ_unload, univ_merge;
    bool worker_selected = false;
    bool town_hall_selected = false;
    bool producer_built = false;
    bool combat_selected = false;
    bool mobile_selected = false;
    bool commandable_selected = false;
    bool authored_layout = false;
    bool opener_keys_old = false;
    bool can_afford_producer = true;
    bool can_afford_supply = true;
    bool can_afford_factory = true;
    bool cancellable_selected = false;
    bool can_afford_engbay = true;
    bool liberator_selected = false;
    bool liberator_ag_selected = false;
    bool templar_merge_selected = false;
    bool burrow_selected = false;
    bool unburrow_selected = false;
    bool burrow_requirement_met = true;
    bool liftoff_selected = false;
    bool liftoff_busy = false;
    bool land_selected = false;
    bool depot_lower_selected = false;
    bool depot_raise_selected = false;
    int depot_lower_slot = -1;
    int depot_raise_slot = -1;
    std::string depot_lower_icon, depot_raise_icon;
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
    kMeleeAbilMove = 31,
};

inline CommandCard build_melee_command_card(const MeleeCommandContext& ctx) {
    CommandCard card;
    std::vector<CardEntry> entries;

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
        e.slot = ctx.univ_merge.slot >= 0 ? ctx.univ_merge.slot : 0;
        e.icon = ctx.univ_merge.icon.empty() ? std::string("btn-ability-protoss-archonwarp")
                                             : ctx.univ_merge.icon;
        e.name = "Merge into Archon";
        e.hotkey = "R";
        e.ability = kMeleeAbilMergeArchon;
        e.requirement_met = true;
        entries.push_back(e);
    }

    if (ctx.burrow_selected) {
        CardEntry e;
        e.slot = ctx.univ_burrow.slot >= 0 ? ctx.univ_burrow.slot : 0;
        e.icon = ctx.univ_burrow.icon.empty() ? std::string("btn-ability-zerg-burrow")
                                              : ctx.univ_burrow.icon;
        e.name = "Burrow";
        e.hotkey = ctx.univ_burrow.hotkey.empty() || ctx.opener_keys_old ? std::string("R") : ctx.univ_burrow.hotkey;
        e.ability = kMeleeAbilBurrow;
        e.requirement_met = ctx.burrow_requirement_met;
        entries.push_back(e);
    }
    if (ctx.unburrow_selected) {
        CardEntry e;
        e.slot = ctx.univ_unburrow.slot >= 0 ? ctx.univ_unburrow.slot : 1;
        e.icon = ctx.univ_unburrow.icon.empty() ? std::string("btn-ability-zerg-burrowup")
                                                : ctx.univ_unburrow.icon;
        e.name = "Unburrow";
        e.hotkey = ctx.univ_unburrow.hotkey.empty() || ctx.opener_keys_old ? std::string("R") : ctx.univ_unburrow.hotkey;
        e.ability = kMeleeAbilUnburrow;
        e.requirement_met = true;
        entries.push_back(e);
    }

    if (ctx.liftoff_selected) {
        CardEntry e;
        e.slot = ctx.univ_liftoff.slot >= 0 ? ctx.univ_liftoff.slot : 2;
        e.icon = ctx.univ_liftoff.icon.empty() ? std::string("btn-ability-terran-liftoff") : ctx.univ_liftoff.icon;
        e.name = "Lift Off";
        e.hotkey = "L";
        e.ability = kMeleeAbilLiftOff;
        e.requirement_met = !ctx.liftoff_busy;
        entries.push_back(e);
    }
    if (ctx.land_selected) {
        CardEntry e;
        e.slot = ctx.univ_liftoff.slot >= 0 ? ctx.univ_liftoff.slot : 2;
        e.icon = ctx.univ_liftoff.icon.empty() ? std::string("btn-ability-terran-land") : ctx.univ_liftoff.icon;
        e.name = "Land";
        e.hotkey = "L";
        e.ability = kMeleeAbilLand;
        e.requirement_met = true;
        entries.push_back(e);
    }

    if (ctx.depot_lower_selected) {
        CardEntry e;
        e.slot = ctx.depot_lower_slot >= 0 ? ctx.depot_lower_slot : 2;
        e.icon = ctx.depot_lower_icon.empty() ? std::string("btn-ability-terran-supplydepotlower") : ctx.depot_lower_icon;
        e.name = "Lower Supply Depot";
        e.hotkey = "R";
        e.ability = kMeleeAbilLowerDepot;
        e.requirement_met = true;
        entries.push_back(e);
    }
    if (ctx.depot_raise_selected) {
        CardEntry e;
        e.slot = ctx.depot_raise_slot >= 0 ? ctx.depot_raise_slot : 2;
        e.icon = ctx.depot_raise_icon.empty() ? std::string("btn-ability-terran-supplydepotraise") : ctx.depot_raise_icon;
        e.name = "Raise Supply Depot";
        e.hotkey = "R";
        e.ability = kMeleeAbilRaiseDepot;
        e.requirement_met = true;
        entries.push_back(e);
    }

    if (ctx.transport_unload_selected) {
        CardEntry e;
        e.slot = ctx.univ_unload.slot >= 0 ? ctx.univ_unload.slot : 0;
        e.icon = ctx.univ_unload.icon.empty() ? std::string("btn-command-unloadall") : ctx.univ_unload.icon;
        e.name = "Unload All";
        e.hotkey = "D";
        e.ability = kMeleeAbilUnloadAll;
        e.requirement_met = true;
        entries.push_back(e);
    }

    if (ctx.commandable_selected && (ctx.mobile_selected || ctx.authored_layout)) {
        struct Univ { int slot; const char* icon; const char* name; const char* hotkey; std::uint32_t ability;
                      const MeleeCommandContext::UniversalCell* authored; };
        const Univ kUniv[] = {
            {-1, "", "Move", "M", kMeleeAbilMove, &ctx.univ_move},
            {10, "btn-command-stop", "Stop", "X", kMeleeAbilStop, &ctx.univ_stop},
            {11, "btn-command-holdposition", "Hold Position", "H", kMeleeAbilHold, &ctx.univ_hold},
            {12, "btn-command-attack", "Attack", "A", kMeleeAbilAttack, &ctx.univ_attack},
            {13, "btn-command-patrol", "Patrol", "P", kMeleeAbilPatrol, &ctx.univ_patrol},
        };
        for (const Univ& u : kUniv) {
            CardEntry e;
            e.slot = u.authored->slot >= 0 ? u.authored->slot
                                           : ((ctx.worker_selected || ctx.authored_layout) ? -1 : u.slot);
            if (e.slot < 0) continue;
            bool clash = false;
            for (const CardEntry& prior : entries)
                if (prior.slot == e.slot) { clash = true; break; }
            if (clash) continue;
            e.icon = u.authored->icon.empty() ? std::string(u.icon) : u.authored->icon;
            e.name = u.authored->name.empty() ? std::string(u.name) : u.authored->name;
            e.hotkey = u.authored->hotkey.empty() ? std::string(u.hotkey) : u.authored->hotkey;
            e.ability = u.ability;
            e.requirement_met = true;
            entries.push_back(e);
        }
    }

    card.fill(entries);
    if (ctx.cancellable_selected && card.slots[CommandCard::kCancelSlot].state == ButtonState::Empty)
        card.set_cancel(ctx.cancel_icon);
    return card;
}

}
