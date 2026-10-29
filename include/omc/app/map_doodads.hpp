#pragma once

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <string>

#include "omc/data/catalog.hpp"
#include "omc/formats/m3.hpp"
#include "omc/formats/map_archive.hpp"

namespace omc::app {

inline constexpr float kDoodadThinMedium = 1.5f;
inline constexpr float kDoodadThinLow = 4.0f;

inline std::uint8_t doodad_thin_level(const data::Catalog* cat, const formats::PlacedDoodad& d, const formats::Model& m) {
    if (!cat || !m.lights.empty()) return 0;
    if (!cat->doodad_nav_footprint_offsets(d.type).empty() || !cat->doodad_shape_footprint_offsets(d.type).empty()) return 0;
    const float ex = (m.bounds_max[0] - m.bounds_min[0]) * std::fabs(d.sx);
    const float ey = (m.bounds_max[1] - m.bounds_min[1]) * std::fabs(d.sy);
    const float ez = (m.bounds_max[2] - m.bounds_min[2]) * std::fabs(d.sz);
    const float size = std::max(ex, std::max(ey, ez));
    if (!(size > 0.0f)) return 0;
    if (size < kDoodadThinMedium) return 1;
    if (size < kDoodadThinLow) return 2;
    return 0;
}

inline void note_body_occupancy(formats::LoadedMap& map, int cx, int cy,
                                const std::vector<std::pair<int, int>>& nav,
                                const std::vector<std::pair<int, int>>& place) {
    const std::size_t n = std::size_t(map.nav.width()) * std::size_t(map.nav.height());
    if (map.body_nav_block.size() != n) map.body_nav_block.assign(n, 0);
    if (map.body_place_block.size() != n) map.body_place_block.assign(n, 0);
    auto mark = [&](std::vector<std::uint8_t>& mask, const std::vector<std::pair<int, int>>& offs) {
        for (const std::pair<int, int>& o : offs) {
            const int x = cx + o.first, y = cy + o.second;
            if (!map.nav.in_bounds(x, y)) continue;
            mask[std::size_t(y) * std::size_t(map.nav.width()) + std::size_t(x)] = 1;
        }
    };
    mark(map.body_nav_block, nav);
    mark(map.body_place_block, place);
}

inline std::size_t stamp_map_doodads(const data::Catalog& cat, formats::LoadedMap& map) {
    if (!map.nav.configured()) return 0;
    const bool floor_anchor = std::getenv("OMC_DOODAD_ANCHOR_OLD") != nullptr;
    std::size_t stamped = 0;
    for (const formats::PlacedDoodad& d : map.objects.doodads) {
        if (d.no_footprint) continue;
        const std::vector<std::pair<int, int>> offs = cat.doodad_nav_footprint_offsets(d.type);
        if (offs.empty()) continue;
        int cx = int(d.x), cy = int(d.y);
        if (!floor_anchor) {
            int x0 = offs[0].first, x1 = x0, y0 = offs[0].second, y1 = y0;
            for (const std::pair<int, int>& o : offs) {
                x0 = std::min(x0, o.first);
                x1 = std::max(x1, o.first);
                y0 = std::min(y0, o.second);
                y1 = std::max(y1, o.second);
            }
            const double ox = double(d.x) - 0.5 * double(x0 + x1 + 1);
            const double oy = double(d.y) - 0.5 * double(y0 + y1 + 1);
            cx = (x1 - x0) % 2 ? int(std::ceil(ox - 0.5)) : int(std::floor(ox + 0.5));
            cy = (y1 - y0) % 2 ? int(std::ceil(oy - 0.5)) : int(std::floor(oy + 0.5));
        }
        note_body_occupancy(map, cx, cy, offs, cat.doodad_shape_footprint_offsets(d.type));
        for (const std::pair<int, int>& o : offs) {
            const int x = cx + o.first, y = cy + o.second;
            if (!map.nav.in_bounds(x, y) || map.nav.is_blocked_cell(x, y)) continue;
            map.nav.set_blocked(x, y, true);
            map.note_block(x, y, formats::LoadedMap::kBlockDoodad);
            ++stamped;
        }
    }
    if (stamped) {
        map.blocked_cells += stamped;
        map.doodad_blocked += stamped;
        map.placement = formats::build_terrain_map(map);
    }
    return stamped;
}

inline std::size_t stamp_map_units(const data::Catalog& cat, formats::LoadedMap& map) {
    if (!map.nav.configured()) return 0;
    std::size_t stamped = 0;
    for (const formats::PlacedUnit& u : map.objects.units) {
        if (cat.unit_stats(u.unit_type).speed != 0) continue;
        const std::vector<std::pair<int, int>> offs = cat.unit_nav_footprint_offsets(u.unit_type);
        if (offs.empty()) continue;
        const int cx = int(u.x), cy = int(u.y);
        note_body_occupancy(map, cx, cy, offs, cat.unit_shape_footprint_offsets(u.unit_type));
        for (const std::pair<int, int>& o : offs) {
            const int x = cx + o.first, y = cy + o.second;
            if (!map.nav.in_bounds(x, y) || map.nav.is_blocked_cell(x, y)) continue;
            map.nav.set_blocked(x, y, true);
            map.note_block(x, y, formats::LoadedMap::kBlockBody);
            ++stamped;
        }
    }
    if (stamped) {
        map.blocked_cells += stamped;
        map.unit_blocked += stamped;
        map.placement = formats::build_terrain_map(map);
    }
    return stamped;
}

}
