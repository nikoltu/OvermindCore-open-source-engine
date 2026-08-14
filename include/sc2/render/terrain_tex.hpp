#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "sc2/formats/m3.hpp"
#include "sc2/formats/terrain.hpp"
#include "sc2/render/scene.hpp"

namespace sc2::casc {
class Storage;
}

namespace sc2::render {

const std::vector<std::string>& tileset_diffuse_candidates();

std::string resolve_tileset_diffuse(const casc::Storage& store);

std::string resolve_map_tileset_diffuse(const casc::Storage& store,
                                        const std::string& tileset,
                                        const std::vector<std::string>& preferred_ids = {});

struct ResolvedLayer {
    std::string terrain_tex;
    std::string diffuse;
    std::string normal;
};

std::vector<ResolvedLayer> resolve_map_layers(const casc::Storage& store,
                                              const std::vector<std::string>& layer_names);

void set_terrain_diffuse(formats::Model& m, const std::string& diffuse_path);

struct GroundImage {
    uint32_t w = 0, h = 0;
    std::vector<uint8_t> rgba;
    bool empty() const { return w == 0 || h == 0 || rgba.empty(); }
};

GroundImage composite_map_ground(const casc::Storage& store, const formats::TextureInfo& info,
                                 const formats::TextureMask& mask,
                                 const std::vector<ResolvedLayer>& layers, uint32_t uv_tiles = 9);

void set_terrain_diffuse_rgba(formats::Model& m, uint32_t w, uint32_t h,
                              std::vector<uint8_t> rgba);

void apply_grid_uvs(formats::Model& m, float uv_tiles);

formats::Model build_terrain_model(float extent, uint32_t divs, float uv_tiles,
                                   const std::string& diffuse_path,
                                   const std::vector<float>& heights = {});

SceneInstance terrain_instance(const formats::Model& model);

std::vector<uint8_t> make_checker_rgba(uint32_t w, uint32_t h, uint32_t cells);

}
