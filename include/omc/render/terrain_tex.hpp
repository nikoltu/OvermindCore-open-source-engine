#pragma once
#include <cmath>

#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

#include "omc/formats/m3.hpp"
#include "omc/formats/terrain.hpp"
#include "omc/render/scene.hpp"

namespace omc::casc {
class Storage;
}

namespace omc::formats {
struct LoadedMap;
}

namespace omc::render {

const std::vector<std::string>& tileset_diffuse_candidates();

std::string resolve_tileset_diffuse(const casc::Storage& store);

std::string resolve_map_tileset_diffuse(const casc::Storage& store,
                                        const std::string& tileset,
                                        const std::vector<std::string>& preferred_ids = {});

inline bool dxt5nm_to_rgb(std::vector<std::uint8_t>& rgba) {
    const std::size_t n = rgba.size() / 4;
    if (n == 0) return false;
    const std::size_t step = n > 4096 ? n / 4096 : 1;
    bool blue_zero = true, grey = true;
    int a_lo = 255, a_hi = 0;
    for (std::size_t i = 0; i < n; i += step) {
        const int r = rgba[i * 4], gch = rgba[i * 4 + 1], b = rgba[i * 4 + 2], a = rgba[i * 4 + 3];
        if (b > 5) blue_zero = false;
        if (r - gch > 8 || gch - r > 8 || b - gch > 8 || gch - b > 8) grey = false;
        a_lo = a < a_lo ? a : a_lo;
        a_hi = a > a_hi ? a : a_hi;
    }
    const bool grey_alpha = !blue_zero && grey && a_hi - a_lo > 8 && !std::getenv("OMC_NO_GREY_NORMALS");
    if (!blue_zero && !grey_alpha) return false;
    if (grey_alpha) {
        for (std::size_t i = 0; i < n; ++i) {
            const float x = float(rgba[i * 4 + 3]) / 255.0f * 2.0f - 1.0f;
            const float y = float(rgba[i * 4 + 1]) / 255.0f * 2.0f - 1.0f;
            float zz = 1.0f - x * x - y * y;
            if (zz < 0.0f) zz = 0.0f;
            rgba[i * 4] = rgba[i * 4 + 3];
            rgba[i * 4 + 2] = std::uint8_t((std::sqrt(zz) * 0.5f + 0.5f) * 255.0f + 0.5f);
        }
        return true;
    }
    for (std::size_t i = 0; i < n; ++i) {
        const float x = float(rgba[i * 4 + 3]) / 255.0f * 2.0f - 1.0f;
        const float y = float(rgba[i * 4 + 1]) / 255.0f * 2.0f - 1.0f;
        float zz = 1.0f - x * x - y * y;
        if (zz < 0.0f) zz = 0.0f;
        const float z = std::sqrt(zz);
        const std::uint8_t height = rgba[i * 4];
        rgba[i * 4] = rgba[i * 4 + 3];
        rgba[i * 4 + 2] = std::uint8_t((z * 0.5f + 0.5f) * 255.0f + 0.5f);
        rgba[i * 4 + 3] = height;
    }
    return true;
}

struct ResolvedLayer {
    std::string terrain_tex;
    std::string diffuse;
    std::string normal;
    std::string heightmap;
    float height_scale = 1.0f, height_offset = 0.0f;
};

bool decode_layer_rgba(const casc::Storage& store, const std::string& diffuse_rel, uint32_t& w,
                       uint32_t& h, std::vector<uint8_t>& out);

std::vector<ResolvedLayer> resolve_map_layers(const casc::Storage& store,
                                              const std::vector<std::string>& layer_names);

void set_terrain_diffuse(formats::Model& m, const std::string& diffuse_path);

struct GroundImage {
    uint32_t w = 0, h = 0;
    std::vector<uint8_t> rgba;
    bool empty() const { return w == 0 || h == 0 || rgba.empty(); }
};

struct LayerImage {
    uint32_t w = 0, h = 0;
    std::vector<uint8_t> rgba;
    bool ok() const { return w != 0 && h != 0 && rgba.size() >= size_t(w) * h * 4; }
};

GroundImage composite_ground_images(const formats::TextureInfo& info,
                                    const formats::TextureMask& mask,
                                    const std::vector<LayerImage>& images, uint32_t uv_tiles = 9);

GroundImage composite_map_ground(const casc::Storage& store, const formats::TextureInfo& info,
                                 const formats::TextureMask& mask,
                                 const std::vector<ResolvedLayer>& layers, uint32_t uv_tiles = 9);

bool set_terrain_splat_gpu(formats::Model& m, const casc::Storage& store,
                           const std::vector<std::string>& layer_names,
                           const formats::TextureMask& mask, float extent_x, float extent_y,
                           const formats::VertexTint* tint = nullptr);

void set_terrain_diffuse_rgba(formats::Model& m, uint32_t w, uint32_t h,
                              std::vector<uint8_t> rgba);

void texture_map_ground(formats::Model& ground, const casc::Storage& store, const formats::LoadedMap& lm);

void apply_grid_uvs(formats::Model& m, float uv_tiles);

void apply_grid_uvs(formats::Model& m, float uv_tiles_u, float uv_tiles_v);

formats::Model build_terrain_model(float extent, uint32_t divs, float uv_tiles,
                                   const std::string& diffuse_path,
                                   const std::vector<float>& heights = {});

SceneInstance terrain_instance(const formats::Model& model);

std::vector<uint8_t> make_checker_rgba(uint32_t w, uint32_t h, uint32_t cells);

}
