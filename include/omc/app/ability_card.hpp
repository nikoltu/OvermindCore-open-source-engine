#pragma once

#include <cstdlib>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "omc/app/tech_gate.hpp"
#include "omc/data/catalog.hpp"

namespace omc::app {

struct AbilityCardEntry {
    int slot = -1;
    int card = 0;
    std::string icon;
    std::string name;
    std::string hotkey;
    float cooldown = 0.0f;
    bool autocast = false;
    std::string ability;
    std::string command;
    std::string face;
    bool requirement_met = true;
    int charges = -1;
};

inline std::vector<AbilityCardEntry> build_ability_card_entries(
    const data::Catalog& cat, const std::string& unit_catalog_id, const data::TechState& st) {
    std::vector<AbilityCardEntry> out;
    if (unit_catalog_id.empty()) return out;
    for (const data::Catalog::AbilityCardButton& b : cat.unit_ability_card_buttons(unit_catalog_id)) {
        if (is_universal_ability(b.ability)) continue;
        if (const data::Catalog::Entry* ce = cat.find("CAbil", b.ability)) {
            if (ce->tag == "CAbilTrain" || ce->tag == "CAbilWarpTrain") continue;
            static const bool kNukeArmOld = std::getenv("OMC_NUKE_ARM_CARD_OLD") != nullptr;
            if (!kNukeArmOld && cat.calldown_magazine(b.ability)) continue;
            if (ce->tag == "CAbilBuild" && !cat.build_spec(b.ability).has_value()) continue;
        }
        const std::string req = cat.button_requirement(b.ability, b.command);
        if (data::requirement_hidden(cat, req, st)) continue;
        AbilityCardEntry e;
        e.ability = b.ability;
        e.command = b.command;
        e.face = b.face;
        e.name = b.face;
        if (auto nm = cat.button_display_name(b.face)) e.name = std::move(*nm);
        if (auto ic = cat.button_icon(b.face)) e.icon = std::move(*ic);
        if (auto hk = cat.button_hotkey(b.face)) e.hotkey = std::move(*hk);
        if (b.row >= 0 && b.col >= 0) e.slot = b.row * 5 + b.col;
        e.card = b.card;
        e.requirement_met = req.empty() || data::eval_requirement(cat, req, "Use", st);
        out.push_back(std::move(e));
    }
    return out;
}

inline std::vector<AbilityCardEntry> build_ability_card_entries(
    const data::Catalog& cat, const std::string& unit_catalog_id,
    const std::vector<std::string>& researched_upgrades,
    const std::unordered_map<std::string, int>& units_complete = {}) {
    return build_ability_card_entries(cat, unit_catalog_id,
                                      tech_state_from(researched_upgrades, units_complete));
}

}
