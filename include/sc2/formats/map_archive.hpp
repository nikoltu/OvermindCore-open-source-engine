#pragma once

#include <algorithm>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "sc2/formats/map_info.hpp"
#include "sc2/formats/mpq.hpp"
#include "sc2/formats/placed_objects.hpp"
#include "sc2/formats/terrain.hpp"
#include "sc2/formats/terrain_grid.hpp"
#include "sc2/sim/nav.hpp"
#include "sc2/sim/world.hpp"

namespace sc2::formats {

struct LoadedMap {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    HeightMap heightmap;
    TerrainGrid terrain;
    sim::NavGrid nav;
    std::size_t blocked_cells = 0;
    PlacedObjects objects;
    MapInfoHeader info;
    std::vector<std::uint8_t> heightmap_raw;
    TerrainQuantize quantize;
    std::string tileset;
    std::string cliff_set;
    std::vector<std::string> cliff_sets;
    std::vector<CliffCell> cliff_cells;
    std::vector<TerrainRamp> ramps;
    std::uint32_t cliff_grid_w = 0, cliff_grid_h = 0;
    std::vector<std::string> tileset_textures;
    TextureInfo texture_info;
    TextureMask texture_mask;
    bool has_splat = false;
    bool has_retail_splat = false;
    CliffMap cliff;
    SyncHeightMap sync_height;
    CellFlags cell_flags;
    std::size_t authored_blocked = 0;
    bool has_cliff_levels = false;
    WaterMap water;
};

inline LoadedMap load_map_terrain(std::vector<std::uint8_t> sc2map_bytes) {
    Mpq arc = Mpq::open(std::move(sc2map_bytes));
    if (!arc.contains("t3HeightMap"))
        throw std::runtime_error("map_archive: no t3HeightMap (not a terrain map?)");

    std::vector<std::uint8_t> hb = arc.read_file("t3HeightMap");

    LoadedMap out;
    out.heightmap = HeightMap::parse(hb);

    TerrainQuantize q;
    if (arc.contains("t3Terrain.xml")) {
        std::vector<std::uint8_t> tx = arc.read_file("t3Terrain.xml");
        q = parse_terrain_quantize(tx);
        out.tileset = parse_terrain_tileset(tx);
        out.cliff_set = parse_terrain_cliffset(tx);
        out.cliff_sets = parse_terrain_cliffsets(tx);
        out.cliff_cells = parse_terrain_cliffcells(tx, out.cliff_grid_w, out.cliff_grid_h);
        out.ramps = parse_terrain_ramps(tx);
        out.tileset_textures = parse_terrain_textures(tx);
    }

    out.terrain = build_terrain_grid(out.heightmap, hb, q);
    out.quantize = q;
    out.heightmap_raw = hb;
    out.width = out.terrain.width;
    out.height = out.terrain.height;
    if (arc.contains("t3CellFlags")) {
        try { out.cell_flags = CellFlags::parse(arc.read_file("t3CellFlags")); }
        catch (const std::exception&) { out.cell_flags = CellFlags{}; }
    }

    out.blocked_cells =
        terrain_grid_to_nav(out.terrain, out.nav, sim::Fixed::from_int(1), sim::Fixed(), sim::Fixed(),
                             out.cell_flags.empty());

    if (arc.contains("t3SyncCliffLevel")) {
        try {
            out.cliff = CliffMap::parse(arc.read_file("t3SyncCliffLevel"));
            if (!out.cliff.empty()) {
                const int nw = out.nav.width(), nh = out.nav.height();
                const int cw = int(out.cliff.width), ch = int(out.cliff.height);
                const int mw = cw < nw ? cw : nw, mh = ch < nh ? ch : nh;
                for (int y = 0; y < mh; ++y)
                    for (int x = 0; x < mw; ++x)
                        out.nav.set_level(x, y, out.cliff.levels[std::size_t(y) * cw + x]);
                if (out.cliff.raw.size() == std::size_t(out.cliff.width) * out.cliff.height) {
                    for (int y = 0; y < mh; ++y)
                        for (int x = 0; x < mw; ++x)
                            if (out.cliff.raw[std::size_t(y) * cw + x] & 63) out.nav.set_ramp(x, y, true);
                } else {
                    out.nav.mark_ramps_from_levels();
                }
                out.has_cliff_levels = out.nav.has_levels();
            }
            if (arc.contains("t3SyncHeightMap")) {
                try { out.sync_height = SyncHeightMap::parse(arc.read_file("t3SyncHeightMap")); }
                catch (const std::exception&) { out.sync_height = SyncHeightMap{}; }
            }

            {
                if (!out.cell_flags.empty()) {
                    const int nw = out.nav.width(), nh = out.nav.height();
                    const int fw = int(out.cell_flags.width), fh = int(out.cell_flags.height);
                    const int mw = fw < nw ? fw : nw, mh = fh < nh ? fh : nh;
                    for (int y = 0; y < mh; ++y) {
                        for (int x = 0; x < mw; ++x) {
                            if (!out.cell_flags.blocked(std::uint32_t(x), std::uint32_t(y))) continue;
                            if (!out.nav.is_blocked_cell(x, y)) {
                                out.nav.set_blocked(x, y, true);
                                ++out.blocked_cells;
                                ++out.authored_blocked;
                            }
                            const std::uint8_t lv = out.nav.level_cell(x, y);
                            const bool wall =
                                (x > 0 && out.nav.level_cell(x - 1, y) != lv) ||
                                (y > 0 && out.nav.level_cell(x, y - 1) != lv) ||
                                (x + 1 < mw && out.nav.level_cell(x + 1, y) != lv) ||
                                (y + 1 < mh && out.nav.level_cell(x, y + 1) != lv);
                            if (wall) out.nav.mark_cliff(x, y, true);
                        }
                    }
                }
            }
        } catch (const std::exception&) {
            out.cliff = CliffMap{};
            out.has_cliff_levels = false;
        }
    }

    {
        const std::uint32_t cell_w = !out.cell_flags.empty() ? out.cell_flags.width : out.cliff.width;
        const std::uint32_t cell_h = !out.cell_flags.empty() ? out.cell_flags.height : out.cliff.height;
        if (cell_w > 0 && cell_h > 0) {
            for (int y = 0; y < out.nav.height(); ++y)
                for (int x = 0; x < out.nav.width(); ++x)
                    if ((std::uint32_t(x) >= cell_w || std::uint32_t(y) >= cell_h) &&
                        !out.nav.is_blocked_cell(x, y)) {
                        out.nav.set_blocked(x, y, true);
                        ++out.blocked_cells;
                    }
        }
    }

    if (arc.contains("t3Water")) {
        try {
            WaterMap wm = WaterMap::parse(arc.read_file("t3Water"));
            if (!wm.empty()) out.water = std::move(wm);
        } catch (const std::exception&) {
            out.water = WaterMap{};
        }
    }

    if (arc.contains("Objects")) out.objects = parse_placed_objects(arc.read_file("Objects"));
    if (arc.contains("MapInfo")) out.info = parse_map_info(arc.read_file("MapInfo"));

    if (arc.contains("t3TextureMasks")) {
        try { out.texture_mask = TextureMask::parse(arc.read_file("t3TextureMasks")); }
        catch (const std::exception&) { out.texture_mask = TextureMask{}; }
    }
    if (arc.contains("t3SyncTextureInfo")) {
        try {
            out.texture_info = TextureInfo::parse(arc.read_file("t3SyncTextureInfo"));
            out.has_splat = !out.texture_mask.empty() &&
                            out.texture_mask.mask_w == out.texture_info.width * 8 &&
                            out.texture_mask.mask_h == out.texture_info.height * 8;
        } catch (const std::exception&) {
            out.texture_info = TextureInfo{};
            out.has_splat = false;
        }
    }
    out.has_retail_splat = !out.has_splat && !out.texture_mask.empty() &&
                           out.tileset_textures.size() >= 2 &&
                           out.texture_mask.mask_w % 8 == 0 && out.texture_mask.mask_h % 8 == 0;
    return out;
}

template <typename StampFn>
std::size_t spawn_placed_units(sim::World& world, const PlacedObjects& objs, StampFn stamp,
                               int mineral_amount = 1500, int gas_amount = 2250) {
    std::size_t n = 0;
    for (const PlacedUnit& pu : objs.units) {
        sim::Unit u;
        u.x = sim::Fixed::from_ratio(int(pu.x * 64.0f), 64);
        u.y = sim::Fixed::from_ratio(int(pu.y * 64.0f), 64);
        u.alive = true;
        if (is_resource_type(pu.unit_type)) {
            u.player = -1;
            const bool gas = pu.unit_type.find("Geyser") != std::string::npos ||
                             pu.unit_type.find("Vespene") != std::string::npos;
            u.resource_left = gas ? gas_amount : mineral_amount;
            u.radius = sim::Fixed::from_ratio(1, 1);
        } else {
            u.player = pu.player;
            u.hp = u.hp_max = sim::Fixed::from_int(1);
            stamp(pu.unit_type, u);
        }
        world.add_unit(u);
        ++n;
    }
    return n;
}

inline std::size_t spawn_placed_units(sim::World& world, const PlacedObjects& objs,
                                      int mineral_amount = 1500, int gas_amount = 2250) {
    return spawn_placed_units(
        world, objs, [](const std::string&, sim::Unit&) {}, mineral_amount, gas_amount);
}

inline Model build_map_mesh(const LoadedMap& map, float cell = 1.0f, std::uint32_t max_dim = 128) {
    const float scale = map.quantize.found ? float(map.quantize.quantize_scale) : 1.0f / 512.0f;
    const float bias = (map.quantize.found ? float(map.quantize.quantize_bias) : 0.0f) +
                       (map.quantize.found ? float(map.quantize.standard_height) : 0.0f);
    return build_terrain_mesh(map.heightmap, map.heightmap_raw, cell, scale, bias, max_dim);
}

inline void fit_mesh_to_playable(Model& mesh, const LoadedMap& map) {
    if (mesh.vertex_count == 0 || map.width < 2 || map.height < 2) return;
    float mnx = 1e30f, mny = 1e30f, mxx = -1e30f, mxy = -1e30f;
    for (std::uint32_t i = 0; i < mesh.vertex_count; ++i) {
        const float x = mesh.positions[i * 3 + 0], y = mesh.positions[i * 3 + 1];
        mnx = x < mnx ? x : mnx; mxx = x > mxx ? x : mxx;
        mny = y < mny ? y : mny; mxy = y > mxy ? y : mxy;
    }
    const float exx = mxx - mnx, exy = mxy - mny;
    if (exx <= 1e-3f || exy <= 1e-3f) return;
    const float sx = float(map.width) / exx, sy = float(map.height) / exy;
    for (std::uint32_t i = 0; i < mesh.vertex_count; ++i) {
        mesh.positions[i * 3 + 0] = (mesh.positions[i * 3 + 0] - mnx) * sx;
        mesh.positions[i * 3 + 1] = (mesh.positions[i * 3 + 1] - mny) * sy;
    }
}

inline Model build_map_mesh_stepped(const LoadedMap& map, float cell = 1.0f,
                                    std::uint32_t max_dim = 128, bool with_walls = true) {
    if (map.cliff.empty()) {
        Model smooth = build_map_mesh(map, cell, max_dim);
        fit_mesh_to_playable(smooth, map);
        return smooth;
    }

    const float scale = map.quantize.found ? float(map.quantize.quantize_scale) : 1.0f / 512.0f;
    const float bias = (map.quantize.found ? float(map.quantize.quantize_bias) : 0.0f) +
                       (map.quantize.found ? float(map.quantize.standard_height) : 0.0f);
    const HeightMap& hm = map.heightmap;
    const std::vector<std::uint8_t>& raw = map.heightmap_raw;
    auto height_at = [&](std::uint32_t col, std::uint32_t row) -> float {
        if (!map.sync_height.empty()) return map.sync_height.world_at(col, row);
        if (hm.width == 0 || hm.height == 0 || hm.stride < 2) return 0.0f;
        if (col >= hm.width) col = hm.width - 1;
        if (row >= hm.height) row = hm.height - 1;
        const std::size_t rec = hm.data_offset + (std::size_t(row) * hm.width + col) * hm.stride;
        if (rec + 1 >= raw.size()) return 0.0f;
        const std::uint16_t h = std::uint16_t(raw[rec]) | std::uint16_t(raw[rec + 1]) << 8;
        return float(h) * scale - bias;
    };

    const CliffMap& cm = map.cliff;
    std::uint8_t maxL = 0;
    for (std::uint8_t v : cm.levels) maxL = v > maxL ? v : maxL;
    std::vector<double> sum(std::size_t(maxL) + 1, 0.0);
    std::vector<std::uint64_t> cnt(std::size_t(maxL) + 1, 0);
    for (std::uint32_t y = 0; y < cm.height; ++y)
        for (std::uint32_t x = 0; x < cm.width; ++x) {
            const std::uint8_t L = cm.levels[std::size_t(y) * cm.width + x];
            sum[L] += height_at(x, y);
            cnt[L] += 1;
        }
    std::vector<float> level_height(std::size_t(maxL) + 1, 0.0f);
    for (std::size_t L = 0; L <= maxL; ++L)
        if (cnt[L]) level_height[L] = float(sum[L] / double(cnt[L]));
    float step = 2.0f;
    {
        float last = 0.0f;
        bool has_last = false;
        std::vector<float> diffs;
        for (std::size_t L = 0; L <= maxL; ++L)
            if (cnt[L]) {
                if (has_last) diffs.push_back(level_height[L] - last);
                last = level_height[L];
                has_last = true;
            }
        if (!diffs.empty()) {
            std::sort(diffs.begin(), diffs.end());
            const float md = diffs[diffs.size() / 2];
            if (md > 1e-3f) step = md;
        }
    }
    float prev = 0.0f;
    bool have_prev = false;
    for (std::size_t L = 0; L <= maxL; ++L) {
        if (cnt[L]) {
            prev = level_height[L];
            have_prev = true;
        } else if (have_prev) {
            prev += step;
            level_height[L] = prev;
        } else {
            level_height[L] = float(L) * step;
        }
    }

    {
        float lo = 1e30f, hi = -1e30f;
        for (std::size_t L = 0; L <= maxL; ++L)
            if (cnt[L]) {
                lo = level_height[L] < lo ? level_height[L] : lo;
                hi = level_height[L] > hi ? level_height[L] : hi;
            }
        if (hi >= lo && hi - lo < 1e-2f) {
            const float s = step > 1e-3f ? step : 2.0f;
            for (std::size_t L = 0; L <= maxL; ++L) level_height[L] = float(L) * s;
            lo = 0.0f;
        }
        if (lo < 1e29f)
            for (std::size_t L = 0; L <= maxL; ++L) level_height[L] -= lo;
    }

    std::vector<std::uint8_t> ramp(std::size_t(cm.width) * cm.height, 0);
    if (cm.raw.size() == std::size_t(cm.width) * cm.height) {
        for (std::size_t i = 0; i < ramp.size(); ++i) ramp[i] = (cm.raw[i] & 63) ? 1 : 0;
    } else {
        auto lvl = [&](int x, int y) -> int {
            if (x < 0 || y < 0 || x >= int(cm.width) || y >= int(cm.height)) return -1;
            return int(cm.levels[std::size_t(y) * cm.width + x]);
        };
        for (int y = 0; y < int(cm.height); ++y)
            for (int x = 0; x < int(cm.width); ++x) {
                const int me = lvl(x, y);
                bool higher = false, lower = false;
                const int nx[4] = {x - 1, x + 1, x, x}, ny[4] = {y, y, y - 1, y + 1};
                for (int k = 0; k < 4; ++k) {
                    const int n = lvl(nx[k], ny[k]);
                    if (n < 0) continue;
                    if (n > me) higher = true;
                    if (n < me) lower = true;
                }
                if (higher && lower) ramp[std::size_t(y) * cm.width + x] = 1;
            }
    }

    Model mesh = build_terrain_mesh_stepped(map.cliff, level_height, cell, max_dim, &ramp, with_walls);
    fit_mesh_to_playable(mesh, map);
    return mesh;
}

inline Model build_map_water(const LoadedMap& map, float cell = 1.0f, float lift = 0.5f) {
    if (map.water.empty()) return Model{};

    const float scale = map.quantize.found ? float(map.quantize.quantize_scale) : 1.0f / 512.0f;
    const float bias = (map.quantize.found ? float(map.quantize.quantize_bias) : 0.0f) +
                       (map.quantize.found ? float(map.quantize.standard_height) : 0.0f);
    const HeightMap& hm = map.heightmap;
    const std::vector<std::uint8_t>& raw = map.heightmap_raw;
    auto height_at = [&](std::uint32_t col, std::uint32_t row) -> float {
        if (hm.width == 0 || hm.height == 0 || hm.stride < 2) return 0.0f;
        if (col >= hm.width) col = hm.width - 1;
        if (row >= hm.height) row = hm.height - 1;
        const std::size_t rec = hm.data_offset + (std::size_t(row) * hm.width + col) * hm.stride;
        if (rec + 1 >= raw.size()) return 0.0f;
        const std::uint16_t h = std::uint16_t(raw[rec]) | std::uint16_t(raw[rec + 1]) << 8;
        return float(h) * scale - bias;
    };

    std::vector<float> level;
    level.reserve(map.water.planes.size());
    for (const WaterPlane& p : map.water.planes) {
        const float minx = p.min_x < p.max_x ? p.min_x : p.max_x;
        const float maxx = p.min_x < p.max_x ? p.max_x : p.min_x;
        const float miny = p.min_y < p.max_y ? p.min_y : p.max_y;
        const float maxy = p.min_y < p.max_y ? p.max_y : p.min_y;
        const std::uint32_t x0 = std::uint32_t(minx < 0.0f ? 0.0f : minx);
        const std::uint32_t y0 = std::uint32_t(miny < 0.0f ? 0.0f : miny);
        const std::uint32_t x1 = std::uint32_t(maxx < 0.0f ? 0.0f : maxx);
        const std::uint32_t y1 = std::uint32_t(maxy < 0.0f ? 0.0f : maxy);
        double sum = 0.0;
        std::uint64_t n = 0;
        for (std::uint32_t y = y0; y <= y1; ++y)
            for (std::uint32_t x = x0; x <= x1; ++x) {
                sum += height_at(x, y);
                ++n;
            }
        level.push_back((n ? float(sum / double(n)) : 0.0f) + lift);
    }
    return build_water_mesh(map.water, level, cell);
}

inline void append_map_water(Model& ground, const LoadedMap& map, float cell = 1.0f) {
    Model w = build_map_water(map, cell);
    if (w.regions.empty()) return;
    const std::uint32_t vbase = std::uint32_t(ground.positions.size() / 3);
    const std::uint32_t ibase = std::uint32_t(ground.indices.size());
    ground.positions.insert(ground.positions.end(), w.positions.begin(), w.positions.end());
    ground.uvs.insert(ground.uvs.end(), w.uvs.begin(), w.uvs.end());
    ground.indices.reserve(ground.indices.size() + w.indices.size());
    for (std::uint16_t idx : w.indices) ground.indices.push_back(idx);
    for (std::size_t r = 0; r < w.regions.size(); ++r) {
        M3Region rg = w.regions[r];
        rg.first_vertex += vbase;
        rg.first_index += ibase;
        ground.regions.push_back(rg);
        ground.region_material.push_back(-1);
    }
    ground.vertex_count = std::uint32_t(ground.positions.size() / 3);
}

inline sim::World build_world(const LoadedMap& map) {
    sim::World world;
    world.nav() = map.nav;
    spawn_placed_units(world, map.objects);
    return world;
}

template <typename StampFn>
inline sim::World build_world(const LoadedMap& map, StampFn stamp) {
    sim::World world;
    world.nav() = map.nav;
    spawn_placed_units(world, map.objects, stamp);
    return world;
}

}
