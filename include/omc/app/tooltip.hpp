#pragma once

#include <cstdio>
#include <string>

namespace omc::app {

struct TooltipInfo {
    std::string name;
    std::string hotkey;
    int minerals = 0;
    int gas = 0;
    int supply = 0;
    float build_time = 0;
    std::string requirement;
    std::string description;
    int kills = -1;
    int rank = -1;
};

inline std::string build_tooltip(const TooltipInfo& t) {
    std::string s = t.name;
    if (!t.hotkey.empty()) s += " (" + t.hotkey + ")";

    std::string cost;
    auto add = [&](const std::string& part) {
        if (part.empty()) return;
        if (!cost.empty()) cost += "  ";
        cost += part;
    };
    if (t.minerals > 0) add("Minerals " + std::to_string(t.minerals));
    if (t.gas > 0) add("Gas " + std::to_string(t.gas));
    if (t.supply > 0) add("Supply " + std::to_string(t.supply));
    if (t.build_time > 0) {
        char buf[32];
        std::snprintf(buf, sizeof buf, "Time %.0fs", double(t.build_time));
        add(buf);
    }
    if (!cost.empty()) s += "\n" + cost;

    if (!t.requirement.empty()) s += "\nRequires: " + t.requirement;
    if (!t.description.empty()) s += "\n" + t.description;
    if (t.kills >= 0) s += "\nKills: " + std::to_string(t.kills);
    if (t.rank >= 0) s += "\nRank: " + std::to_string(t.rank);
    return s;
}

}
