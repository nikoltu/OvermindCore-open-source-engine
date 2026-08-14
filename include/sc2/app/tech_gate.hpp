#pragma once

#include <string>
#include <unordered_map>
#include <vector>

#include "sc2/data/catalog.hpp"
#include "sc2/data/requirements.hpp"

namespace sc2::app {

inline data::TechState tech_state_from(const std::vector<std::string>& researched_upgrades,
                                       const std::unordered_map<std::string, int>& units_complete =
                                           {}) {
    data::TechState st;
    for (const std::string& up : researched_upgrades)
        if (!up.empty()) st.upgrades[up] = 1;
    st.units_complete = units_complete;
    return st;
}

inline bool requirement_met(const data::Catalog& cat, const std::string& req_id,
                            const std::vector<std::string>& researched_upgrades,
                            const std::unordered_map<std::string, int>& units_complete = {},
                            const std::string& index = "Use") {
    if (req_id.empty()) return true;
    return data::eval_requirement(cat, req_id, index,
                                  tech_state_from(researched_upgrades, units_complete));
}

inline bool ability_available(const data::Catalog& cat, const std::string& abil_id,
                              const std::string& cmd_index,
                              const std::vector<std::string>& researched_upgrades,
                              const std::unordered_map<std::string, int>& units_complete = {}) {
    return requirement_met(cat, cat.ability_command_requirement(abil_id, cmd_index),
                           researched_upgrades, units_complete);
}

inline bool is_universal_ability(const std::string& abil_id) {
    return abil_id == "move" || abil_id == "stop" || abil_id == "attack";
}

inline std::vector<std::pair<std::string, std::string>> castable_abilities(
    const data::Catalog& cat, const std::string& unit_catalog_id,
    const std::vector<std::string>& researched_upgrades,
    const std::unordered_map<std::string, int>& units_complete = {}) {
    std::vector<std::pair<std::string, std::string>> out;
    if (unit_catalog_id.empty()) return out;
    for (const auto& [abil, cmd] : cat.unit_ability_commands(unit_catalog_id)) {
        if (is_universal_ability(abil)) continue;
        if (ability_available(cat, abil, cmd, researched_upgrades, units_complete))
            out.emplace_back(abil, cmd);
    }
    return out;
}

}
