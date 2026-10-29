#pragma once
#include <cstdlib>
#include <string>

#include "omc/data/catalog.hpp"

namespace omc::app {

inline std::string kill_rank_key(const data::Catalog& cat, const std::string& unit_id, int kills, bool* hero_out = nullptr) {
    static const bool no_hero_rank = std::getenv("OMC_NO_HERO_RANK") != nullptr;
    const bool hero = !no_hero_rank && !unit_id.empty() && cat.unit_flag(unit_id, "Hero");
    if (hero_out) *hero_out = hero;
    if (hero) return "UnitKillRank/Hero";
    std::string key = "UnitKillRank/Rank1";
    if (std::getenv("OMC_KILL_RANK_FIRST")) return key;
    for (const char* entry : {"CoreDflt", "Dflt"})
        for (const core::XmlNode& kn : cat.resolved("CGameUI", entry)) {
            if (kn.name != "UnitKillRank") continue;
            const std::string text = kn.attr_or("Text", "");
            if (!text.empty() && std::atoi(kn.attr_or("MinKills", "0").c_str()) <= kills) key = text;
        }
    return key;
}

inline const std::string* kill_rank_name(const data::Catalog& cat, const std::string& unit_id, int kills, const char* race_sfx) {
    bool hero = false;
    const std::string key = kill_rank_key(cat, unit_id, kills, &hero);
    const std::string* rn = cat.strings().find(key + "_" + race_sfx);
    if (!rn && hero) rn = cat.strings().find(key);
    return rn;
}

}
