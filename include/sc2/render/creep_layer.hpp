#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "sc2/formats/m3.hpp"
#include "sc2/sim/creep.hpp"

namespace sc2::casc {
class Storage;
}

namespace sc2::render {

void set_model_diffuse(formats::Model& m, const std::string& diffuse_path);

const std::vector<std::string>& creep_texture_candidates();

std::string resolve_creep_diffuse(const casc::Storage& store);

formats::Model build_creep_model(const sim::CreepGrid& grid,
                                 const std::function<float(float, float)>& ground_z,
                                 float z_offset, float uv_tiles_per_cell,
                                 const std::string& diffuse_path);

std::vector<std::uint8_t> creep_model_cells(const formats::Model& model, const sim::CreepGrid& grid);

std::vector<std::uint8_t> creep_grid_cells(const sim::CreepGrid& grid);

}
