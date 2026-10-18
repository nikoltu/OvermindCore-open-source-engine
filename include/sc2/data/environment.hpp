#pragma once

#include <map>
#include <string>

#include "sc2/data/catalog.hpp"

namespace sc2::data {

struct EnvDirLight {
    bool present = false;
    double color[3] = {1, 1, 1};
    double color_multiplier = 1.0;
    double spec_color[3] = {1, 1, 1};
    double spec_multiplier = 1.0;
    double direction[3] = {0, 0, -1};
    bool use_ambient_occlusion = false;
};

struct EnvLight {
    bool found = false;
    std::string light_id;
    std::string resolution;

    bool has_hdr = false;
    double hdr_exposure = 0;
    double hdr_white_point = 0;

    bool has_key_dir = false;
    double key_dir[3] = {0, 0, 0};

    bool has_ambient = false;
    double ambient_color[3] = {0, 0, 0};
    EnvDirLight key, fill, back;

    std::map<std::string, double> params;

    double param(const std::string& key, double dflt) const {
        auto it = params.find(key);
        return it == params.end() ? dflt : it->second;
    }
};

EnvLight resolve_environment_light(const Catalog& cat, const std::string& tileset);
EnvLight resolve_map_environment(const Catalog& cat, const std::string& tileset);
void overlay_map_light(EnvLight& env, const std::vector<uint8_t>& light_xml);
void overlay_map_terrain(EnvLight& env, const std::vector<uint8_t>& terrain_xml, const std::string& tileset);

std::map<std::string, double> camera_params(const Catalog& cat, const std::string& camera_id);

struct TeamColor {
    int slot = -1;
    float diffuse[4] = {0, 0, 0, 1};
    float selected[4] = {0, 0, 0, 1};
    float emissive[4] = {0, 0, 0, -1};
    float attacked[4] = {1, 0, 0, 1};
};

std::vector<TeamColor> team_colors(const Catalog& cat);

}
