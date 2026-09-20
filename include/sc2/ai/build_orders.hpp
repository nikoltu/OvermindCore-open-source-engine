#pragma once
namespace sc2::ai {

struct StockEntry {
    const char* unit;
    int count;
};

inline const StockEntry* stock_open_timing(const char* race, int& count) {
    static const StockEntry kTerran[] = {
        {"CommandCenter", 1}, {"SCV", 14}, {"SupplyDepot", 1}, {"SCV", 16}, {"Barracks", 1}, {"SCV", 17},
        {"Refinery", 1}, {"SCV", 18}, {"Marine", 1}, {"OrbitalCommand", 1}, {"SupplyDepot", 2}, {"Marine", 2},
        {"Barracks", 2}, {"BarracksTechLab", 1}, {"Refinery", 2},
    };
    static const StockEntry kProtoss[] = {
        {"Nexus", 1}, {"Probe", 14}, {"Pylon", 1}, {"Probe", 16}, {"Gateway", 1}, {"Probe", 17},
        {"Assimilator", 1}, {"Probe", 18}, {"Pylon", 2}, {"CyberneticsCore", 1}, {"Stalker", 1}, {"Gateway", 2},
        {"Assimilator", 2},
    };
    static const StockEntry kZerg[] = {
        {"Hatchery", 1}, {"Drone", 6}, {"Overlord", 1}, {"Drone", 13}, {"Overlord", 2}, {"Drone", 16},
        {"SpawningPool", 1}, {"Drone", 17}, {"Extractor", 1}, {"Overlord", 3}, {"Queen", 1}, {"Zergling", 4},
        {"Drone", 18}, {"RoachWarren", 1}, {"Extractor", 2},
    };
    const char* r = race ? race : "";
    if (r[0] == 't') { count = int(sizeof(kTerran) / sizeof(kTerran[0])); return kTerran; }
    if (r[0] == 'p') { count = int(sizeof(kProtoss) / sizeof(kProtoss[0])); return kProtoss; }
    if (r[0] == 'z') { count = int(sizeof(kZerg) / sizeof(kZerg[0])); return kZerg; }
    count = 0;
    return nullptr;
}

}
