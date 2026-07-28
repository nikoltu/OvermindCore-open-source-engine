#include "sc2/formats/terrain_grid.hpp"

#include <cmath>

namespace sc2::formats {

TerrainGrid build_terrain_grid(const HeightMap& hm, const std::vector<uint8_t>& data,
                               const TerrainQuantize& q, float slope_threshold) {
    TerrainGrid g;
    g.width = hm.width;
    g.height = hm.height;
    const size_t n = g.cell_count();
    g.heights.assign(n, 0.0f);
    g.blocked.assign(n, 0);
    g.flags.assign(n, 0);
    if (g.width == 0 || g.height == 0) return g;

    const size_t stride = hm.stride;
    const bool have_flag = stride > 4;

    for (uint32_t y = 0; y < g.height; ++y)
        for (uint32_t x = 0; x < g.width; ++x) {
            const size_t rec = hm.data_offset + (size_t(y) * g.width + x) * stride;
            const size_t i = g.idx(x, y);
            if (rec + 1 < data.size()) {
                uint16_t raw = uint16_t(data[rec]) | uint16_t(data[rec + 1]) << 8;
                g.heights[i] = float(raw) * q.quantize_scale - q.quantize_bias;
            }
            if (have_flag && rec + 4 < data.size())
                g.flags[i] = uint8_t(data[rec + 4] & 0x03);
        }

    for (uint32_t y = 0; y < g.height; ++y)
        for (uint32_t x = 0; x < g.width; ++x) {
            const size_t i = g.idx(x, y);
            const float z = g.heights[i];
            float maxd = 0.0f;
            if (x + 1 < g.width) {
                float d = std::fabs(g.heights[i + 1] - z);
                if (d > maxd) maxd = d;
            }
            if (x > 0) {
                float d = std::fabs(g.heights[i - 1] - z);
                if (d > maxd) maxd = d;
            }
            if (y + 1 < g.height) {
                float d = std::fabs(g.heights[i + g.width] - z);
                if (d > maxd) maxd = d;
            }
            if (y > 0) {
                float d = std::fabs(g.heights[i - g.width] - z);
                if (d > maxd) maxd = d;
            }
            g.blocked[i] = (maxd > slope_threshold) ? uint8_t(1) : uint8_t(0);
        }
    return g;
}

size_t terrain_grid_to_nav(const TerrainGrid& tg, sim::NavGrid& out,
                           sim::Fixed cell_size, sim::Fixed origin_x,
                           sim::Fixed origin_y) {
    out.configure(int(tg.width), int(tg.height), cell_size, origin_x, origin_y);
    size_t blocked = 0;
    for (uint32_t y = 0; y < tg.height; ++y)
        for (uint32_t x = 0; x < tg.width; ++x)
            if (tg.blocked[tg.idx(x, y)]) {
                out.block_cell(int(x), int(y));
                ++blocked;
            }
    return blocked;
}

}
