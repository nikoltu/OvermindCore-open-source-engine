#pragma once

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
