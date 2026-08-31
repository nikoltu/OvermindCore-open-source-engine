#pragma once

#include <array>
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

struct TerrainUvTiling {
    float u = 9.0f, v = 9.0f;
    bool found = false;
};
TerrainUvTiling parse_terrain_uvtiling(const std::vector<uint8_t>& t3terrain_xml);

std::string parse_terrain_tileset(const std::vector<uint8_t>& t3terrain_xml);

std::vector<std::string> parse_terrain_textures(const std::vector<uint8_t>& t3terrain_xml);

std::string parse_terrain_cliffset(const std::vector<uint8_t>& t3terrain_xml);

std::vector<std::string> parse_terrain_cliffsets(const std::vector<uint8_t>& t3terrain_xml);

struct CliffCell {
    uint32_t index = 0;
    uint8_t cid = 0;
    uint8_t cvar = 0;
    uint8_t flag = 0;
};
std::vector<CliffCell> parse_terrain_cliffcells(const std::vector<uint8_t>& t3terrain_xml,
                                                uint32_t& grid_w, uint32_t& grid_h);

struct RampFrame {
    float ux = 0, uy = 0;
    float rx = 0, ry = 0;
    float cx = 0, cy = 0;
    float w = 0, h = 0;
    bool used() const { return w != 0.0f || h != 0.0f; }
};

struct TerrainRamp {
    uint32_t dir = 0;
    uint32_t hi = 0, lo = 0;
    uint32_t cid = 0;
    RampFrame left_lo, left_hi, right_lo, right_hi, base, mid;
    uint32_t left_lo_var = 0xFFFFFFFFu, left_hi_var = 0xFFFFFFFFu;
    uint32_t right_lo_var = 0xFFFFFFFFu, right_hi_var = 0xFFFFFFFFu;
};

std::vector<TerrainRamp> parse_terrain_ramps(const std::vector<uint8_t>& t3terrain_xml);

Model build_terrain_mesh(const HeightMap& hm, const std::vector<uint8_t>& data,
                         float cell = 1.0f, float quantize_scale = 1.0f / 512.0f,
                         float quantize_bias = 0.0f, uint32_t max_dim = 128);

struct SyncHeightMap {
    uint32_t version = 0;
    uint32_t width = 0, height = 0;
    std::vector<int16_t> height_i16;
    std::vector<uint16_t> level;
    bool empty() const { return width == 0 || height == 0 || height_i16.empty(); }
    float world_at(uint32_t col, uint32_t row) const {
        if (empty()) return 0.0f;
        if (col >= width) col = width - 1;
        if (row >= height) row = height - 1;
        return float(height_i16[size_t(row) * width + col]) / 256.0f;
    }
    static SyncHeightMap parse(const std::vector<uint8_t>& data);
};

struct CellFlags {
    uint32_t width = 0, height = 0;
    std::vector<uint8_t> cells;
    bool empty() const { return width == 0 || height == 0 || cells.empty(); }
    bool blocked(uint32_t x, uint32_t y) const {
        if (empty() || x >= width || y >= height) return false;
        return (cells[size_t(y) * width + x] & 0x01) != 0;
    }
    static CellFlags parse(const std::vector<uint8_t>& data);
};

struct CliffMap;

Model build_terrain_mesh_stepped(const CliffMap& cliff,
                                 const std::vector<float>& level_height, float cell = 1.0f,
                                 const std::vector<uint8_t>* ramp = nullptr, bool with_walls = true);

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
    std::vector<uint16_t> raw;
    bool empty() const { return width == 0 || height == 0 || levels.empty(); }
    static CliffMap parse(const std::vector<uint8_t>& data);
};

struct WaterPlane {
    std::string type;
    float min_x = 0, min_y = 0, max_x = 0, max_y = 0;
    std::array<float, 6> params{};
};

struct WaterMap {
    uint32_t version = 0;
    std::vector<WaterPlane> planes;
    bool empty() const { return planes.empty(); }
    static WaterMap parse(const std::vector<uint8_t>& data);
};

Model build_water_mesh(const WaterMap& water, const std::vector<float>& level, float cell = 1.0f);

struct TextureMask {
    uint32_t version = 0;
    uint32_t mask_w = 0, mask_h = 0;
    std::vector<uint8_t> texels;
    bool empty() const { return mask_w == 0 || mask_h == 0; }
    uint32_t quad_w() const { return mask_w ? (mask_w + 127) / 128 : 0; }
    uint32_t quad_h() const {
        const uint32_t qw = quad_w();
        if (qw == 0) return 0;
        return uint32_t((texels.size() / 8 / 4096) / qw);
    }
    bool planar_ok() const { return mask_w > 0 && quad_w() > 0 && quad_h() > 0; }
    uint8_t layer_weight(uint32_t layer, uint32_t x, uint32_t y) const;
    static TextureMask parse(const std::vector<uint8_t>& data);
};

}
