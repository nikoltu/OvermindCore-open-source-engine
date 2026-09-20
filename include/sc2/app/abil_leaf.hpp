#pragma once
#include <algorithm>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "sc2/app/ability.hpp"
#include "sc2/app/tech_gate.hpp"
#include "sc2/data/catalog.hpp"
#include "sc2/sim/world.hpp"

namespace sc2::app {

inline const char* effect_kind_name(sim::EffectKind k) {
    switch (k) {
        case sim::EffectKind::Damage: return "Damage";
        case sim::EffectKind::Heal: return "Heal";
        case sim::EffectKind::Buff: return "Buff";
        case sim::EffectKind::ModifyEnergy: return "ModifyEnergy";
        case sim::EffectKind::DrainEnergy: return "DrainEnergy";
        case sim::EffectKind::ApplyBehavior: return "ApplyBehavior";
        case sim::EffectKind::Teleport: return "Teleport";
        case sim::EffectKind::CreatePersistent: return "CreatePersistent";
        case sim::EffectKind::CreateUnit: return "CreateUnit";
        case sim::EffectKind::Morph: return "Morph";
        case sim::EffectKind::RemoveBehavior: return "RemoveBehavior";
        case sim::EffectKind::Set: return "Set";
        case sim::EffectKind::EnumArea: return "EnumArea";
        case sim::EffectKind::Switch: return "Switch";
        case sim::EffectKind::IssueOrder: return "IssueOrder";
        case sim::EffectKind::CreateHealer: return "CreateHealer";
        case sim::EffectKind::WeaponDamage: return "WeaponDamage";
        case sim::EffectKind::Reveal: return "Reveal";
        case sim::EffectKind::Force: return "Force";
        case sim::EffectKind::PhaseShift: return "PhaseShift";
        case sim::EffectKind::Inject: return "Inject";
        case sim::EffectKind::ModifyVitals: return "ModifyVitals";
        case sim::EffectKind::DestroyPersistent: return "DestroyPersistent";
        case sim::EffectKind::CancelOrder: return "CancelOrder";
        case sim::EffectKind::TransferBehavior: return "TransferBehavior";
    }
    return "?";
}

struct AbilLeafVerdict {
    std::string id;
    std::string cls;
    bool resolved = false;
    sim::EffectKind kind = sim::EffectKind::Damage;
    std::string kind_name;
    bool tree = false;
    std::size_t tree_nodes = 0;
    std::string route;
};

inline std::string abil_route(const data::Catalog& cat, const std::string& id, const std::string& cls) {
    if (is_universal_ability(id)) return "universal command row (app::is_universal_ability)";
    if (cls == "CAbilTrain")
        return "train card (selection_train_entries -> human_train)";
    if (cls == "CAbilWarpTrain")
        return "warp card (selection_train_entries -> human_warp_train -> World::warp_train)";
    if (cls == "CAbilBuild") {
        if (cat.build_spec(id).has_value()) return std::string();
        return "build card (selection_build_entries -> human_build)";
    }
    if (cls == "CAbilResearch")
        return "research command (melee_game.cpp research interception)";
    if (cls == "CAbilRally") return "rally command (human_rally)";
    if (cls == "CAbilMerge") return "merge command (human_merge)";
    if (cls == "CAbilQueue") return "cancel/queue command";
    if (cls == "CAbilArmMagazine") return "magazine (spawn.cpp attack_spawn / interceptor)";
    if (cls == "CAbilAugment") return "sim::ChargeSystem";
    if (cls == "CAbilHarvest") return "harvest order (spawn.cpp worker classification)";
    if (cls == "CAbilTransport") return "transport (Catalog::transport_ability)";
    if (cls == "CAbilBehavior") return "behaviour toggle (melee_game.cpp behaviour interception)";
    return std::string();
}

inline std::vector<AbilLeafVerdict> sweep_abil_leaves(const data::Catalog& cat,
                                                      const std::vector<std::string>& ids) {
    std::vector<AbilLeafVerdict> out;
    out.reserve(ids.size());
    for (const std::string& id : ids) {
        AbilLeafVerdict v;
        v.id = id;
        if (const data::Catalog::Entry* e = cat.find("CAbil", id)) v.cls = e->tag;
        v.route = v.cls.empty() ? std::string() : abil_route(cat, id, v.cls);
        if (std::optional<AbilityBuild> b = make_ability(cat, id)) {
            v.resolved = true;
            v.tree = !b->tree.empty();
            v.tree_nodes = b->tree.size();
            if (v.tree && b->tree_root >= 0 &&
                static_cast<std::size_t>(b->tree_root) < b->tree.size())
                v.kind = b->tree[static_cast<std::size_t>(b->tree_root)].kind;
            else
                v.kind = b->def.kind;
            v.kind_name = effect_kind_name(v.kind);
        } else {
            v.kind_name = "UNRESOLVED";
        }
        out.push_back(std::move(v));
    }
    return out;
}

inline std::vector<std::string> melee_card_abilities(const data::Catalog& cat, const std::string& race) {
    std::set<std::string> ids;
    for (const std::string& u : cat.ids("CUnit")) {
        const std::optional<std::string> r = cat.scalar("CUnit", u, "Race");
        if (!r || *r != race) continue;
        const std::optional<std::string> mob = cat.scalar("CUnit", u, "Mob");
        if (!mob || *mob != "Multiplayer") continue;
        for (const data::Catalog::CommandCardLayout& c : cat.unit_command_cards(u))
            for (const data::Catalog::CommandCardSlot& sl : c.slots) {
                if (sl.type != "AbilCmd") continue;
                if (sl.ability.empty()) continue;
                if (!cat.find("CAbil", sl.ability)) continue;
                ids.insert(sl.ability);
            }
    }
    return std::vector<std::string>(ids.begin(), ids.end());
}

inline std::vector<std::string> melee_bodies(const data::Catalog& cat, const std::string& race) {
    std::vector<std::string> out;
    for (const std::string& u : cat.ids("CUnit")) {
        const std::optional<std::string> r = cat.scalar("CUnit", u, "Race");
        if (!r || *r != race) continue;
        const std::optional<std::string> mob = cat.scalar("CUnit", u, "Mob");
        if (!mob || *mob != "Multiplayer") continue;
        out.push_back(u);
    }
    std::sort(out.begin(), out.end());
    return out;
}

}
