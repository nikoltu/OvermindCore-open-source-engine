#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <string>
#include <utility>
#include <vector>

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

using FootprintRings = std::vector<std::vector<std::pair<double, double>>>;

inline FootprintRings footprint_cell_squares(const std::vector<std::pair<int, int>>& cells) {
    FootprintRings out;
    if (cells.empty()) return out;
    int x0 = cells[0].first, x1 = x0, y0 = cells[0].second, y1 = y0;
    for (const std::pair<int, int>& c : cells) {
        x0 = std::min(x0, c.first);
        x1 = std::max(x1, c.first);
        y0 = std::min(y0, c.second);
        y1 = std::max(y1, c.second);
    }
    const double mx = 0.5 * double(x0 + x1 + 1), my = 0.5 * double(y0 + y1 + 1);
    for (const std::pair<int, int>& c : cells) {
        const double a = double(c.first) - mx, b = double(c.second) - my;
        out.push_back({{a, b}, {a + 1.0, b}, {a + 1.0, b + 1.0}, {a, b + 1.0}});
    }
    return out;
}

inline bool footprint_layers_blank(const data::Catalog& cat, const std::string& fp) {
    bool any_layer = false;
    for (const core::XmlNode& layer : cat.resolved("CFootprint", fp)) {
        if (layer.name != "Layers") continue;
        any_layer = true;
        for (const core::XmlNode& row : layer.children) {
            if (row.name != "Rows") continue;
            const std::string* v = row.attr("value");
            for (const char ch : v ? *v : row.text)
                if (ch != '.' && ch != ' ' && ch != '\t' && ch != '\r' && ch != '\n') return false;
        }
    }
    return any_layer;
}

inline bool footprint_boosts_cliff(const data::Catalog& cat, const std::string& fp) {
    for (const core::XmlNode& layer : cat.resolved("CFootprint", fp)) {
        if (layer.name != "Layers" || layer.attr_or("index", "") != "Place") continue;
        for (const core::XmlNode& set : layer.children) {
            if (set.name != "Sets") continue;
            for (const core::XmlNode& f : set.children)
                if (f.name == "Positive" && f.attr_or("index", "") == "BoostCliff" && f.attr_or("value", "0") != "0")
                    return true;
        }
    }
    return false;
}

inline FootprintRings doodad_footprint_rings(const data::Catalog& cat, const std::string& fp) {
    if (cat.footprint_has_layer(fp, "Pathing")) {
        const std::vector<std::pair<int, int>> cells = cat.pathing_footprint_offsets(fp, "Pathing");
        if (!cells.empty()) return footprint_cell_squares(cells);
    }
    if (footprint_layers_blank(cat, fp) && std::getenv("OMC_FOOTPRINT_BLANK_LAYERS_OLD") == nullptr) return {};
    FootprintRings rings = cat.footprint_shape_rings(fp);
    if (!rings.empty()) return rings;
    return footprint_cell_squares(cat.pathing_footprint_offsets(fp));
}

inline constexpr std::int64_t kRasterScale = 65536;

using RasterRings = std::vector<std::vector<std::pair<std::int64_t, std::int64_t>>>;

inline std::int64_t raster_units(double v) { return std::llround(v * double(kRasterScale)); }

inline std::int64_t raster_floor_cell(std::int64_t v) {
    return v >= 0 ? v / kRasterScale : -((-v + kRasterScale - 1) / kRasterScale);
}

inline bool raster_point_on_edge(const RasterRings& rings, std::int64_t px, std::int64_t py) {
    for (const std::vector<std::pair<std::int64_t, std::int64_t>>& ring : rings)
        for (std::size_t i = 0; i < ring.size(); ++i) {
            const std::pair<std::int64_t, std::int64_t>& a = ring[i];
            const std::pair<std::int64_t, std::int64_t>& b = ring[(i + 1) % ring.size()];
            if (a == b) continue;
            const std::int64_t cross = (b.first - a.first) * (py - a.second) - (b.second - a.second) * (px - a.first);
            if (cross != 0) continue;
            if (px >= std::min(a.first, b.first) && px <= std::max(a.first, b.first) && py >= std::min(a.second, b.second) &&
                py <= std::max(a.second, b.second))
                return true;
        }
    return false;
}

inline bool raster_point_inside(const RasterRings& rings, std::int64_t px, std::int64_t py) {
    if (raster_point_on_edge(rings, px, py)) return false;
    bool in = false;
    for (const std::vector<std::pair<std::int64_t, std::int64_t>>& ring : rings)
        for (std::size_t i = 0, j = ring.size() - 1; i < ring.size(); j = i++) {
            const std::int64_t xi = ring[i].first, yi = ring[i].second;
            const std::int64_t xj = ring[j].first, yj = ring[j].second;
            if ((yi > py) == (yj > py)) continue;
            const std::int64_t cross = (xj - xi) * (py - yi) - (px - xi) * (yj - yi);
            if (yj > yi ? cross > 0 : cross < 0) in = !in;
        }
    return in;
}

inline bool footprint_point_inside(const FootprintRings& rings, double px, double py) {
    RasterRings r;
    for (const std::vector<std::pair<double, double>>& ring : rings) {
        r.emplace_back();
        for (const std::pair<double, double>& v : ring) r.back().emplace_back(raster_units(v.first), raster_units(v.second));
    }
    return raster_point_inside(r, raster_units(px), raster_units(py));
}

inline std::vector<std::pair<int, int>> rasterize_footprint(const FootprintRings& local, double x, double y,
                                                            double rotation) {
    std::vector<std::pair<int, int>> cells;
    if (local.empty()) return cells;
    const double quarter = 1.5707963267948966;
    const int k = ((int(std::floor(rotation / quarter + 0.5)) % 4) + 4) % 4;
    const std::int64_t ox = raster_units(x), oy = raster_units(y);
    RasterRings rings;
    std::int64_t minx = 0, miny = 0, maxx = 0, maxy = 0;
    bool first = true;
    for (const std::vector<std::pair<double, double>>& ring : local) {
        rings.emplace_back();
        for (const std::pair<double, double>& lv : ring) {
            std::pair<std::int64_t, std::int64_t> v{raster_units(lv.first), raster_units(lv.second)};
            for (int i = 0; i < k; ++i) v = {-v.second, v.first};
            v.first += ox;
            v.second += oy;
            if (first || v.first < minx) minx = v.first;
            if (first || v.first > maxx) maxx = v.first;
            if (first || v.second < miny) miny = v.second;
            if (first || v.second > maxy) maxy = v.second;
            first = false;
            rings.back().push_back(v);
        }
    }
    static const std::int64_t kSample[4][2] = {{kRasterScale / 4, kRasterScale / 4},
                                               {3 * kRasterScale / 4, kRasterScale / 4},
                                               {kRasterScale / 4, 3 * kRasterScale / 4},
                                               {3 * kRasterScale / 4, 3 * kRasterScale / 4}};
    const std::int64_t cy1 = raster_floor_cell(maxy + kRasterScale - 1), cx1 = raster_floor_cell(maxx + kRasterScale - 1);
    for (std::int64_t cy = raster_floor_cell(miny) - 1; cy <= cy1; ++cy)
        for (std::int64_t cx = raster_floor_cell(minx) - 1; cx <= cx1; ++cx)
            for (const auto& smp : kSample)
                if (raster_point_inside(rings, cx * kRasterScale + smp[0], cy * kRasterScale + smp[1])) {
                    cells.emplace_back(int(cx), int(cy));
                    break;
                }
    return cells;
}

inline std::size_t stamp_map_doodads_placed(const data::Catalog& cat, formats::LoadedMap& map) {
    std::size_t stamped = 0, place_only = 0;
    const std::size_t n = std::size_t(map.nav.width()) * std::size_t(map.nav.height());
    for (const formats::PlacedDoodad& d : map.objects.doodads) {
        if (d.no_footprint) continue;
        const std::optional<std::string> fp = cat.scalar("CActorDoodad", d.type, "Footprint");
        if (!fp || fp->empty()) continue;
        if (footprint_boosts_cliff(cat, *fp) && std::getenv("OMC_FOOTPRINT_BRIDGE_OLD") == nullptr) {
            const std::vector<std::pair<int, int>> place =
                rasterize_footprint(footprint_cell_squares(cat.pathing_footprint_offsets(*fp)), d.x, d.y, 0.0);
            if (map.place_only_block.size() != n) map.place_only_block.assign(n, 0);
            for (const std::pair<int, int>& c : place) {
                if (!map.nav.in_bounds(c.first, c.second)) continue;
                const std::size_t i = std::size_t(c.second) * std::size_t(map.nav.width()) + std::size_t(c.first);
                if (!map.place_only_block[i]) ++place_only;
                map.place_only_block[i] = 1;
            }
            continue;
        }
        const std::vector<std::pair<int, int>> nav = rasterize_footprint(doodad_footprint_rings(cat, *fp), d.x, d.y, d.rotation);
        if (nav.empty()) continue;
        if (map.body_nav_block.size() != n) map.body_nav_block.assign(n, 0);
        if (map.body_place_block.size() != n) map.body_place_block.assign(n, 0);
        for (const std::pair<int, int>& c : nav) {
            if (!map.nav.in_bounds(c.first, c.second)) continue;
            const std::size_t i = std::size_t(c.second) * std::size_t(map.nav.width()) + std::size_t(c.first);
            map.body_nav_block[i] = 1;
            map.body_place_block[i] = 1;
            if (map.nav.is_blocked_cell(c.first, c.second)) continue;
            map.nav.set_blocked(c.first, c.second, true);
            map.note_block(c.first, c.second, formats::LoadedMap::kBlockDoodad);
            ++stamped;
        }
    }
    if (stamped || place_only) {
        map.blocked_cells += stamped;
        map.doodad_blocked += stamped;
        map.placement = formats::build_terrain_map(map);
    }
    return stamped;
}

inline std::size_t stamp_map_doodads(const data::Catalog& cat, formats::LoadedMap& map) {
    if (!map.nav.configured()) return 0;
    if (std::getenv("OMC_DOODAD_FOOTPRINT_OLD") == nullptr) return stamp_map_doodads_placed(cat, map);
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
