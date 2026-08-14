#pragma once

#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "sc2/app/tech_gate.hpp"
#include "sc2/data/catalog.hpp"

namespace sc2::app {

struct AbilityCardEntry {
    int slot = -1;
    std::string icon;
    std::string name;
    std::string ability;
    std::string command;
    bool requirement_met = true;
};

inline std::vector<AbilityCardEntry> build_ability_card_entries(
    const data::Catalog& cat, const std::string& unit_catalog_id,
    const std::vector<std::string>& researched_upgrades,
    const std::unordered_map<std::string, int>& units_complete = {}) {
    std::vector<AbilityCardEntry> out;
    if (unit_catalog_id.empty()) return out;
    for (const data::Catalog::AbilityCardButton& b : cat.unit_ability_card_buttons(unit_catalog_id)) {
        if (is_universal_ability(b.ability)) continue;
        AbilityCardEntry e;
        e.ability = b.ability;
        e.command = b.command;
        e.name = b.face;
        if (auto ic = cat.button_icon(b.face)) e.icon = std::move(*ic);
        if (b.row >= 0 && b.col >= 0) e.slot = b.row * 5 + b.col;
        e.requirement_met =
            ability_available(cat, b.ability, b.command, researched_upgrades, units_complete);
        out.push_back(std::move(e));
    }
    return out;
}

}
