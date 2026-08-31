#pragma once

#include <string>

#include "sc2/data/catalog.hpp"
#include "sc2/formats/map_archive.hpp"

namespace sc2::app {

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
    std::size_t stamped = 0;
    for (const formats::PlacedDoodad& d : map.objects.doodads) {
        if (d.no_footprint) continue;
        const std::vector<std::pair<int, int>> offs = cat.doodad_nav_footprint_offsets(d.type);
        if (offs.empty()) continue;
        const int cx = int(d.x), cy = int(d.y);
        note_body_occupancy(map, cx, cy, offs, cat.doodad_shape_footprint_offsets(d.type));
        for (const std::pair<int, int>& o : offs) {
            const int x = cx + o.first, y = cy + o.second;
            if (!map.nav.in_bounds(x, y) || map.nav.is_blocked_cell(x, y)) continue;
            map.nav.set_blocked(x, y, true);
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
