#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace omc::ai::hd {

struct Row {
    std::string unit;
    int count = 0;
};

struct Build {
    std::string id;
    std::string style;
    std::vector<Row> rows;
    int deadline_s = 0;
};

struct Pick {
    std::string build;
    int w_random = 0, w_protoss = 0, w_terran = 0, w_zerg = 0;
};

struct Menu {
    std::string style;
    std::vector<Pick> picks;
};

struct RaceTables {
    std::vector<Build> openings, mids, lates;
    std::vector<Menu> open_menus, mid_menus, late_menus;
    bool empty() const { return openings.empty(); }
};

const char* race_name(int race_index);

std::string shared_script_path(int race_index);

std::string build_script_path(int race_index);

RaceTables parse_race(int race_index, const std::string& shared_script, const std::string& build_script);

std::uint64_t digest(const RaceTables& t);

}
