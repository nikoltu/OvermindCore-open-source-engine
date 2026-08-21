#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "sc2/formats/terrain.hpp"
#include "sc2/sim/nav.hpp"

namespace sc2::formats {

struct TerrainGrid {
    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<float> heights;
    std::vector<uint8_t> blocked;
    std::vector<uint8_t> flags;

    size_t cell_count() const { return size_t(width) * size_t(height); }
    size_t idx(uint32_t x, uint32_t y) const {
        return size_t(y) * size_t(width) + size_t(x);
    }
    bool in_bounds(uint32_t x, uint32_t y) const { return x < width && y < height; }

    bool is_blocked(uint32_t x, uint32_t y) const {
        return in_bounds(x, y) && blocked[idx(x, y)] != 0;
    }
    bool walkable(uint32_t x, uint32_t y) const {
        return in_bounds(x, y) && blocked[idx(x, y)] == 0;
    }
    uint8_t flag_at(uint32_t x, uint32_t y) const {
        return in_bounds(x, y) ? flags[idx(x, y)] : uint8_t(0);
    }
    float height_at(uint32_t x, uint32_t y) const {
        return in_bounds(x, y) ? heights[idx(x, y)] : 0.0f;
    }

    size_t blocked_count() const {
        size_t n = 0;
        for (uint8_t b : blocked) n += (b != 0);
        return n;
    }
};

TerrainGrid build_terrain_grid(const HeightMap& hm, const std::vector<uint8_t>& data,
                               const TerrainQuantize& q, float slope_threshold = 1.0f);

size_t terrain_grid_to_nav(const TerrainGrid& tg, sim::NavGrid& out,
                           sim::Fixed cell_size = sim::Fixed::from_int(1),
                           sim::Fixed origin_x = sim::Fixed(),
                           sim::Fixed origin_y = sim::Fixed(), bool apply_slope = true);

}
