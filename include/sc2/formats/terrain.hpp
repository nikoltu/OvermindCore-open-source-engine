#pragma once

#include <cstdint>
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

Model build_terrain_mesh(const HeightMap& hm, const std::vector<uint8_t>& data,
                         float cell = 1.0f, float quantize_scale = 1.0f / 512.0f,
                         float quantize_bias = 0.0f, uint32_t max_dim = 128);

}
