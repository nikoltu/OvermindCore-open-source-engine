#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "sc2/formats/m3.hpp"

namespace sc2::formats {

struct HeightMap {
    uint32_t version = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t stride = 0;
    size_t data_offset = 32;

    size_t vertex_count() const { return size_t(width) * height; }

    static HeightMap parse(const std::vector<uint8_t>& data);
};

struct TerrainQuantize {
    float quantize_scale = 1.0f / 512.0f;
    float quantize_bias = 0.0f;
    float standard_height = 0.0f;
    bool found = false;
};

TerrainQuantize parse_terrain_quantize(const std::vector<uint8_t>& t3terrain_xml);

std::string parse_terrain_tileset(const std::vector<uint8_t>& t3terrain_xml);

std::vector<std::string> parse_terrain_textures(const std::vector<uint8_t>& t3terrain_xml);

Model build_terrain_mesh(const HeightMap& hm, const std::vector<uint8_t>& data,
                         float cell = 1.0f, float quantize_scale = 1.0f / 512.0f,
                         float quantize_bias = 0.0f, uint32_t max_dim = 128);

struct TextureInfo {
    uint32_t version = 0;
    uint32_t width = 0, height = 0;
    uint32_t field3 = 0;
    std::vector<std::string> layer_names;
    std::vector<uint8_t> slots;
    bool empty() const { return width == 0 || height == 0; }
    static TextureInfo parse(const std::vector<uint8_t>& data);
};

struct CliffMap {
    uint32_t version = 0;
    uint32_t width = 0, height = 0;
    std::vector<uint8_t> levels;
    bool empty() const { return width == 0 || height == 0 || levels.empty(); }
    static CliffMap parse(const std::vector<uint8_t>& data);
};

struct TextureMask {
    uint32_t version = 0;
    uint32_t mask_w = 0, mask_h = 0;
    std::vector<uint8_t> texels;
    bool empty() const { return mask_w == 0 || mask_h == 0; }
    static TextureMask parse(const std::vector<uint8_t>& data);
};

}
