#pragma once

#include <string>

#include "sc2/data/catalog.hpp"

namespace sc2::data {

struct EnvLight {
    bool found = false;
    std::string light_id;
    std::string resolution;

    bool has_hdr = false;
    double hdr_exposure = 0;
    double hdr_white_point = 0;

    bool has_key_dir = false;
    double key_dir[3] = {0, 0, 0};
};

EnvLight resolve_environment_light(const Catalog& cat, const std::string& tileset);

struct TeamColor {
    int slot = -1;
    float diffuse[4] = {0, 0, 0, 1};
    float selected[4] = {0, 0, 0, 1};
};

std::vector<TeamColor> team_colors(const Catalog& cat);

}
