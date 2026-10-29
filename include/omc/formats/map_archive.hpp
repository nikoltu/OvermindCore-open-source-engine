#pragma once

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <map>
#include <stdexcept>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "omc/formats/hard_tile.hpp"
#include "omc/formats/map_info.hpp"
#include "omc/formats/mpq.hpp"
#include "omc/formats/placed_objects.hpp"
#include "omc/formats/terrain.hpp"
#include "omc/formats/tga.hpp"
#include "omc/formats/terrain_grid.hpp"
#include "omc/sim/nav.hpp"
#include "omc/sim/terrain_map.hpp"

namespace omc::formats {

struct ScriptedCamera {
    bool found = false;
    std::string id;
    float distance = 0.0f;
    float pitch_deg = 0.0f;
    float fov_deg = 0.0f;
    float yaw_deg = 180.0f;
};

inline ScriptedCamera parse_scripted_camera(const std::vector<std::uint8_t>& script_bytes,
                                            const std::vector<std::uint8_t>& objects_bytes) {
    ScriptedCamera out;
    const std::string script(script_bytes.begin(), script_bytes.end());
    const std::string objects(objects_bytes.begin(), objects_bytes.end());
    const std::string call = "CameraApplyInfo(", from = "CameraInfoFromId(";
    std::string id;
    for (std::size_t at = script.find(call); at != std::string::npos; at = script.find(call, at + 1)) {
        const std::size_t eol = script.find('\n', at);
        const std::size_t f = script.find(from, at);
        if (f == std::string::npos || (eol != std::string::npos && f > eol)) continue;
        std::size_t b = f + from.size(), e = b;
        while (e < script.size() && script[e] >= '0' && script[e] <= '9') ++e;
        if (e > b) id = script.substr(b, e - b);
    }
    if (id.empty()) return out;
    const std::size_t o0 = objects.find("<ObjectCamera Id=\"" + id + "\"");
    if (o0 == std::string::npos) return out;
    std::size_t o1 = objects.find("</ObjectCamera>", o0);
    if (o1 == std::string::npos) o1 = objects.size();
    const std::string body = objects.substr(o0, o1 - o0);
    auto value = [&](const char* index, float& dst) {
        const std::string key = std::string("<CameraValue Index=\"") + index + "\" Value=\"";
        const std::size_t k = body.find(key);
        if (k == std::string::npos) return false;
        dst = float(std::atof(body.c_str() + k + key.size()));
        return true;
    };
    if (!value("Distance", out.distance) || !value("Pitch", out.pitch_deg) || !value("FieldOfView", out.fov_deg)) return out;
    value("Yaw", out.yaw_deg);
    if (out.distance <= 0.0f || out.fov_deg <= 0.0f) return out;
    out.id = id;
    out.found = true;
    return out;
}

struct LoadedMap {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    HeightMap heightmap;
    TerrainGrid terrain;
    sim::NavGrid nav;
    sim::TerrainMap placement;
    std::size_t blocked_cells = 0;
    std::vector<std::uint8_t> minimap_rgba;
    std::uint32_t minimap_w = 0, minimap_h = 0;
    std::vector<std::uint8_t> body_nav_block;
    std::vector<std::uint8_t> body_place_block;
    PlacedObjects objects;
    ScriptedCamera scripted_camera;
    MapInfoHeader info;
    std::vector<std::uint8_t> heightmap_raw;
    TerrainQuantize quantize;
    TerrainUvTiling uv_tiling;
    std::string tileset;
    std::string cliff_set;
    std::vector<std::string> cliff_sets;
    std::vector<CliffCell> cliff_cells;
    std::vector<TerrainRamp> ramps;
    std::uint32_t cliff_grid_w = 0, cliff_grid_h = 0;
    std::vector<std::string> tileset_textures;
    TextureInfo texture_info;
    TextureMask texture_mask;
    VertexTint vertex_tint;
    struct FluffDoodad {
        float x = 0, y = 0;
        std::uint8_t slot = 0;
    };
    std::vector<FluffDoodad> fluff;
    bool has_splat = false;
    bool has_retail_splat = false;
    CliffMap cliff;
    SyncHeightMap sync_height;
    CellFlags cell_flags;
    std::size_t authored_blocked = 0;
    PaintedPathing painted;
    std::uint32_t painted_blocked = 0;
    std::size_t ramp_rescued = 0;
    std::size_t halo_reopened = 0;
    std::size_t doodad_blocked = 0;
    std::size_t unit_blocked = 0;
    std::size_t out_of_bounds_blocked = 0;
    std::size_t below_zero_blocked = 0;
    std::size_t void_tier_blocked = 0;
    enum BlockSource : std::uint8_t {
        kBlockSlope = 1 << 0,
        kBlockAuthored = 1 << 1,
        kBlockOutside = 1 << 2,
        kBlockBelowZero = 1 << 3,
        kBlockVoidTier = 1 << 4,
        kBlockDoodad = 1 << 5,
        kBlockBody = 1 << 6,
        kBlockPainted = 1 << 7,
    };
    std::vector<std::uint8_t> block_source;
    void note_block(int x, int y, std::uint8_t src) {
        if (block_source.empty() || !nav.in_bounds(x, y)) return;
        block_source[std::size_t(y) * std::size_t(nav.width()) + std::size_t(x)] |= src;
    }
    bool has_cliff_levels = false;
    WaterMap water;
    std::vector<HardTile> hard_tiles;
    std::vector<std::pair<std::string, std::vector<std::uint8_t>>> game_data;
    const std::vector<std::uint8_t>* game_data_file(const std::string& leaf) const {
        for (const auto& kv : game_data)
            if (kv.first == leaf) return &kv.second;
        return nullptr;
    }

    std::uint32_t cell_width() const {
        return info.width ? info.width : (width ? width - 1 : 0);
    }
    std::uint32_t cell_height() const {
        return info.height ? info.height : (height ? height - 1 : 0);
    }
};

inline sim::TerrainMap build_terrain_map(const LoadedMap& map);

inline float map_vertex_height(const LoadedMap& map, std::uint32_t col, std::uint32_t row);

inline bool measure_lowest_tier_height(const LoadedMap& map, float& lowest_height, float& tier_step);

inline LoadedMap load_map_terrain(std::vector<std::uint8_t> map_bytes) {
    Mpq arc = Mpq::open(std::move(map_bytes));
    if (!arc.contains("t3HeightMap"))
        throw std::runtime_error("map_archive: no t3HeightMap (not a terrain map?)");

    std::vector<std::uint8_t> hb = arc.read_file("t3HeightMap");

    LoadedMap out;
    out.heightmap = HeightMap::parse(hb);

    if (arc.contains("Minimap.tga")) {
        try {
            TgaInfo ti;
            std::vector<std::uint8_t> px = decode_tga(arc.read_file("Minimap.tga"), ti);
            if (ti.width && ti.height && px.size() == std::size_t(ti.width) * ti.height * 4) {
                out.minimap_rgba = std::move(px);
                out.minimap_w = ti.width;
                out.minimap_h = ti.height;
            }
        } catch (const std::exception&) {   }
    }

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
        out.uv_tiling = parse_terrain_uvtiling(tx);
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
    if (arc.contains("PaintedPathingLayer")) {
        try { out.painted = PaintedPathing::parse(arc.read_file("PaintedPathingLayer")); }
        catch (const std::exception&) { out.painted = PaintedPathing{}; }
    }

    out.blocked_cells =
        terrain_grid_to_nav(out.terrain, out.nav, sim::Fixed::from_int(1), sim::Fixed(), sim::Fixed(),
                             out.cell_flags.empty());
    out.block_source.assign(std::size_t(out.nav.width()) * std::size_t(out.nav.height()), 0);
    for (int y = 0; y < out.nav.height(); ++y)
        for (int x = 0; x < out.nav.width(); ++x)
            if (out.nav.is_blocked_cell(x, y)) out.note_block(x, y, LoadedMap::kBlockSlope);

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
                if (!out.painted.empty()) {
                    const int nw = out.nav.width(), nh = out.nav.height();
                    const int pw = int(out.width) < nw ? int(out.width) : nw, ph = int(out.height) < nh ? int(out.height) : nh;
                    for (int y = 0; y < ph; ++y)
                        for (int x = 0; x < pw; ++x) {
                            if (!out.painted.cell_unpathable(std::uint32_t(x), std::uint32_t(y))) continue;
                            if (!out.nav.is_blocked_cell(x, y)) {
                                out.nav.set_blocked(x, y, true);
                                ++out.blocked_cells;
                                ++out.painted_blocked;
                            }
                            out.note_block(x, y, LoadedMap::kBlockPainted);
                        }
                }
                if (!out.cell_flags.empty()) {
                    const int nw = out.nav.width(), nh = out.nav.height();
                    const int fw = int(out.cell_flags.width), fh = int(out.cell_flags.height);
                    const int mw = fw < nw ? fw : nw, mh = fh < nh ? fh : nh;
                    const bool have_ramp_mask =
                        out.cliff.raw.size() ==
                        std::size_t(out.cliff.width) * std::size_t(out.cliff.height);
                    const int cw2 = int(out.cliff.width);
                    const bool halo_old = std::getenv("OMC_HALO_OLD") != nullptr;
                    auto ramp_cell = [&](int cx, int cy) {
                        return have_ramp_mask && cx >= 0 && cy >= 0 && cx < cw2 && cy < int(out.cliff.height) &&
                               (out.cliff.raw[std::size_t(cy) * std::size_t(cw2) + std::size_t(cx)] & 63) != 0;
                    };
                    std::vector<std::uint8_t> ramp_open_cluster;
                    if (!halo_old && have_ramp_mask) {
                        ramp_open_cluster.assign(std::size_t(mw) * std::size_t(mh), 0);
                        std::vector<std::uint8_t> seen(std::size_t(mw) * std::size_t(mh), 0);
                        std::vector<std::pair<int, int>> stack, members;
                        for (int sy = 0; sy < mh; ++sy)
                            for (int sx = 0; sx < mw; ++sx) {
                                if (seen[std::size_t(sy) * mw + sx] || !ramp_cell(sx, sy)) continue;
                                seen[std::size_t(sy) * mw + sx] = 1;
                                stack.assign(1, {sx, sy});
                                members.clear();
                                bool any_open = false;
                                while (!stack.empty()) {
                                    const auto [cx, cy] = stack.back();
                                    stack.pop_back();
                                    members.push_back({cx, cy});
                                    if (!out.cell_flags.blocked(std::uint32_t(cx), std::uint32_t(cy))) any_open = true;
                                    const int ndx[4] = {-1, 1, 0, 0}, ndy[4] = {0, 0, -1, 1};
                                    for (int k = 0; k < 4; ++k) {
                                        const int nx = cx + ndx[k], ny = cy + ndy[k];
                                        if (nx < 0 || ny < 0 || nx >= mw || ny >= mh) continue;
                                        if (seen[std::size_t(ny) * mw + nx] || !ramp_cell(nx, ny)) continue;
                                        seen[std::size_t(ny) * mw + nx] = 1;
                                        stack.push_back({nx, ny});
                                    }
                                }
                                if (any_open)
                                    for (const auto& m : members) ramp_open_cluster[std::size_t(m.second) * mw + m.first] = 1;
                            }
                    }
                    for (int y = 0; y < mh; ++y) {
                        for (int x = 0; x < mw; ++x) {
                            if (!out.cell_flags.blocked(std::uint32_t(x), std::uint32_t(y))) continue;
                            if (ramp_cell(x, y)) {
                                bool rescue = true;
                                if (!halo_old && !ramp_open_cluster.empty() && ramp_open_cluster[std::size_t(y) * mw + x]) {
                                    int inner = 0, cliff_side = 0;
                                    const int ndx[4] = {-1, 1, 0, 0}, ndy[4] = {0, 0, -1, 1};
                                    for (int k = 0; k < 4; ++k) {
                                        const int nx = x + ndx[k], ny = y + ndy[k];
                                        if (nx < 0 || ny < 0) continue;
                                        const bool nb = out.cell_flags.blocked(std::uint32_t(nx), std::uint32_t(ny));
                                        if (ramp_cell(nx, ny)) {
                                            if (!nb) ++inner;
                                        } else if (nb) {
                                            ++cliff_side;
                                        }
                                    }
                                    rescue = inner >= 1 && cliff_side <= 1;
                                }
                                if (rescue) {
                                    ++out.ramp_rescued;
                                    continue;
                                }
                            }
                            if (!out.nav.is_blocked_cell(x, y)) {
                                out.nav.set_blocked(x, y, true);
                                ++out.blocked_cells;
                                ++out.authored_blocked;
                            }
                            out.note_block(x, y, LoadedMap::kBlockAuthored);
                            const std::uint8_t lv = out.nav.level_cell(x, y);
                            const bool wall =
                                (x > 0 && out.nav.level_cell(x - 1, y) != lv) ||
                                (y > 0 && out.nav.level_cell(x, y - 1) != lv) ||
                                (x + 1 < mw && out.nav.level_cell(x + 1, y) != lv) ||
                                (y + 1 < mh && out.nav.level_cell(x, y + 1) != lv);
                            if (wall) out.nav.mark_cliff(x, y, true);
                        }
                    }
                    if (!out.sync_height.empty() && out.sync_height.level.size() ==
                                                        std::size_t(out.sync_height.width) * out.sync_height.height) {
                        const int vw = int(out.sync_height.width), vh = int(out.sync_height.height);
                        std::vector<std::uint8_t> was_free(std::size_t(mw) * std::size_t(mh), 0);
                        for (int y = 0; y < mh; ++y)
                            for (int x = 0; x < mw; ++x) was_free[std::size_t(y) * mw + x] = out.nav.is_blocked_cell(x, y) ? 0 : 1;
                        auto vlevel = [&](int vx, int vy) { return out.sync_height.level[std::size_t(vy) * vw + vx]; };
                        auto vheight = [&](int vx, int vy) { return int(out.sync_height.height_i16[std::size_t(vy) * vw + vx]); };
                        const bool heights_ok = out.sync_height.height_i16.size() == out.sync_height.level.size();
                        if (!halo_old && heights_ok) {
                            const int kPlateauRadius = 4, kRimRiseSum = 320, kFlatSpan = 192;
                            const bool corner_old = std::getenv("OMC_HALO_CORNER_OLD") != nullptr;
                            std::vector<int> csum(std::size_t(mw) * std::size_t(mh), 0);
                            std::vector<int> clevel(std::size_t(mw) * std::size_t(mh), -1);
                            for (int y = 0; y < mh && y + 1 < vh; ++y)
                                for (int x = 0; x < mw && x + 1 < vw; ++x) {
                                    const std::size_t i = std::size_t(y) * mw + x;
                                    csum[i] = vheight(x, y) + vheight(x + 1, y) + vheight(x, y + 1) + vheight(x + 1, y + 1);
                                    const int l0 = vlevel(x, y);
                                    if (vlevel(x + 1, y) == l0 && vlevel(x, y + 1) == l0 && vlevel(x + 1, y + 1) == l0) clevel[i] = l0;
                                }
                            std::vector<int> plateau;
                            for (int y = 0; y < mh; ++y)
                                for (int x = 0; x < mw; ++x) {
                                    if (!out.cell_flags.blocked(std::uint32_t(x), std::uint32_t(y))) continue;
                                    if (!out.painted.empty() && out.painted.cell_unpathable(std::uint32_t(x), std::uint32_t(y))) continue;
                                    if (!out.nav.is_blocked_cell(x, y)) continue;
                                    if (ramp_cell(x, y)) continue;
                                    if (out.block_source[std::size_t(y) * std::size_t(out.nav.width()) + x] & ~LoadedMap::kBlockAuthored) continue;
                                    if (x + 1 >= vw || y + 1 >= vh) continue;
                                    const std::size_t i = std::size_t(y) * mw + x;
                                    bool open = false;
                                    if (clevel[i] < 0) {
                                        const int hs[4] = {vheight(x, y), vheight(x + 1, y), vheight(x, y + 1), vheight(x + 1, y + 1)};
                                        open = *std::max_element(hs, hs + 4) - *std::min_element(hs, hs + 4) <= kFlatSpan;
                                        if (open && !corner_old) {
                                            const int ls[4] = {vlevel(x, y), vlevel(x + 1, y), vlevel(x, y + 1), vlevel(x + 1, y + 1)};
                                            const int lo = *std::min_element(ls, ls + 4);
                                            int high = 0;
                                            for (int k = 0; k < 4; ++k) high += ls[k] > lo ? 1 : 0;
                                            const bool sealed =
                                                out.cell_flags.blocked(std::uint32_t(x - 1), std::uint32_t(y)) &&
                                                out.cell_flags.blocked(std::uint32_t(x + 1), std::uint32_t(y)) &&
                                                out.cell_flags.blocked(std::uint32_t(x), std::uint32_t(y - 1)) &&
                                                out.cell_flags.blocked(std::uint32_t(x), std::uint32_t(y + 1));
                                            if (high == 3 && sealed) open = false;
                                        }
                                    } else {
                                        const int lv = clevel[i];
                                        bool edge = false;
                                        for (int vy = std::max(0, y - 1); vy <= std::min(vh - 1, y + 2) && !edge; ++vy)
                                            for (int vx = std::max(0, x - 1); vx <= std::min(vw - 1, x + 2) && !edge; ++vx)
                                                if (vlevel(vx, vy) != lv) edge = true;
                                        if (!edge) continue;
                                        plateau.clear();
                                        for (int py = std::max(0, y - kPlateauRadius); py <= std::min(mh - 1, y + kPlateauRadius); ++py)
                                            for (int px = std::max(0, x - kPlateauRadius); px <= std::min(mw - 1, x + kPlateauRadius); ++px) {
                                                const std::size_t j = std::size_t(py) * mw + px;
                                                if (clevel[j] != lv) continue;
                                                if (out.cell_flags.blocked(std::uint32_t(px), std::uint32_t(py))) continue;
                                                if (!out.painted.empty() && out.painted.cell_unpathable(std::uint32_t(px), std::uint32_t(py))) continue;
                                                plateau.push_back(csum[j]);
                                            }
                                        if (plateau.empty()) continue;
                                        std::nth_element(plateau.begin(), plateau.begin() + plateau.size() / 2, plateau.end());
                                        open = csum[i] - plateau[plateau.size() / 2] <= kRimRiseSum;
                                    }
                                    if (!open) continue;
                                    out.nav.set_blocked(x, y, false);
                                    out.nav.mark_cliff(x, y, false);
                                    if (out.blocked_cells > 0) --out.blocked_cells;
                                    ++out.halo_reopened;
                                }
                        }
                        for (int y = 0; y < mh && (halo_old || !heights_ok); ++y)
                            for (int x = 0; x < mw; ++x) {
                                if (!out.cell_flags.blocked(std::uint32_t(x), std::uint32_t(y))) continue;
                                if (!out.painted.empty() && out.painted.cell_unpathable(std::uint32_t(x), std::uint32_t(y))) continue;
                                if (!out.nav.is_blocked_cell(x, y)) continue;
                                if (x + 1 >= vw || y + 1 >= vh) continue;
                                const std::uint16_t l0 = vlevel(x, y);
                                if (vlevel(x + 1, y) != l0 || vlevel(x, y + 1) != l0 || vlevel(x + 1, y + 1) != l0) continue;
                                const bool touches_free =
                                    (x > 0 && was_free[std::size_t(y) * mw + x - 1]) || (x + 1 < mw && was_free[std::size_t(y) * mw + x + 1]) ||
                                    (y > 0 && was_free[std::size_t(y - 1) * mw + x]) || (y + 1 < mh && was_free[std::size_t(y + 1) * mw + x]);
                                if (!touches_free) continue;
                                out.nav.set_blocked(x, y, false);
                                out.nav.mark_cliff(x, y, false);
                                if (out.blocked_cells > 0) --out.blocked_cells;
                                ++out.halo_reopened;
                            }
                    }
                }
            }
        } catch (const std::exception&) {
            out.cliff = CliffMap{};
            out.has_cliff_levels = false;
        }
    }

    if (arc.contains("MapInfo")) {
        try { out.info = parse_map_info(arc.read_file("MapInfo")); }
        catch (const std::exception&) { out.info = MapInfoHeader{}; }
    }

    {
        const std::uint32_t cell_w = !out.cell_flags.empty() ? out.cell_flags.width : out.cliff.width;
        const std::uint32_t cell_h = !out.cell_flags.empty() ? out.cell_flags.height : out.cliff.height;
        const bool bounded = out.info.has_playable_area();
        if (cell_w > 0 && cell_h > 0) {
            for (int y = 0; y < out.nav.height(); ++y)
                for (int x = 0; x < out.nav.width(); ++x) {
                    const bool overhang =
                        std::uint32_t(x) >= cell_w || std::uint32_t(y) >= cell_h;
                    const bool outside =
                        overhang || (bounded && !out.info.in_playable_area(std::uint32_t(x),
                                                                          std::uint32_t(y)));
                    if (!outside) continue;
                    out.note_block(x, y, LoadedMap::kBlockOutside);
                    if (out.nav.is_blocked_cell(x, y)) continue;
                    out.nav.set_blocked(x, y, true);
                    ++out.blocked_cells;
                    if (!overhang) ++out.out_of_bounds_blocked;
                }
        }
    }

    {
        for (int y = 0; y < out.nav.height(); ++y)
            for (int x = 0; x < out.nav.width(); ++x) {
                if (out.nav.is_blocked_cell(x, y)) continue;
                if (map_vertex_height(out, std::uint32_t(x), std::uint32_t(y)) >= 0.0f) continue;
                out.nav.set_blocked(x, y, true);
                out.note_block(x, y, LoadedMap::kBlockBelowZero);
                ++out.blocked_cells;
                ++out.below_zero_blocked;
            }
    }

    {
        float lowest = 0.0f, step = 2.0f;
        if (measure_lowest_tier_height(out, lowest, step)) {
            const float threshold = lowest - step;
            for (int y = 0; y < out.nav.height(); ++y)
                for (int x = 0; x < out.nav.width(); ++x) {
                    if (out.nav.is_blocked_cell(x, y)) continue;
                    if (out.nav.level_cell(x, y) != 0) continue;
                    if (map_vertex_height(out, std::uint32_t(x), std::uint32_t(y)) >= threshold)
                        continue;
                    out.nav.set_blocked(x, y, true);
                    out.note_block(x, y, LoadedMap::kBlockVoidTier);
                    ++out.blocked_cells;
                    ++out.void_tier_blocked;
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
    for (const std::string& name : arc.list()) {
        std::string low = name;
        for (char& c : low) c = c == '\\' ? '/' : char(std::tolower(static_cast<unsigned char>(c)));
        const std::size_t gd = low.find("base.sc2data/gamedata/");
        if (gd == std::string::npos || low.size() < 4 || low.compare(low.size() - 4, 4, ".xml") != 0) continue;
        try {
            out.game_data.emplace_back(name.substr(gd + 22), arc.read_file(name));
        } catch (const std::exception&) {
        }
    }
    if (arc.contains("t3HardTile")) {
        try {
            out.hard_tiles = parse_hard_tiles(arc.read_file("t3HardTile"));
        } catch (const std::exception&) {
            out.hard_tiles.clear();
        }
    }

    if (arc.contains("Objects")) out.objects = parse_placed_objects(arc.read_file("Objects"));
    if (arc.contains("Objects") && arc.contains("MapScript.galaxy")) {
        try { out.scripted_camera = parse_scripted_camera(arc.read_file("MapScript.galaxy"), arc.read_file("Objects")); }
        catch (const std::exception&) { out.scripted_camera = ScriptedCamera{}; }
    }

    out.placement = build_terrain_map(out);

    if (arc.contains("t3VertCol")) {
        try { out.vertex_tint = VertexTint::parse(arc.read_file("t3VertCol")); }
        catch (const std::exception&) { out.vertex_tint = VertexTint{}; }
    }
    if (arc.contains("t3FluffDoodad")) {
        try {
            const std::vector<std::uint8_t> fb = arc.read_file("t3FluffDoodad");
            if (fb.size() >= 28 && fb[0] == 'D' && fb[1] == 'L' && fb[2] == 'F' && fb[3] == 'T') {
                std::uint32_t count = 0;
                std::memcpy(&count, fb.data() + 24, 4);
                if (std::size_t(count) * 9 + 28 <= fb.size()) {
                    out.fluff.resize(count);
                    for (std::uint32_t i = 0; i < count; ++i) {
                        const std::uint8_t* r = fb.data() + 28 + std::size_t(i) * 9;
                        std::memcpy(&out.fluff[i].x, r, 4);
                        std::memcpy(&out.fluff[i].y, r + 4, 4);
                        out.fluff[i].slot = r[8];
                    }
                }
            }
        } catch (const std::exception&) { out.fluff.clear(); }
    }
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
    const float sx = float(map.width - 1) / exx, sy = float(map.height - 1) / exy;
    for (std::uint32_t i = 0; i < mesh.vertex_count; ++i) {
        mesh.positions[i * 3 + 0] = (mesh.positions[i * 3 + 0] - mnx) * sx;
        mesh.positions[i * 3 + 1] = (mesh.positions[i * 3 + 1] - mny) * sy;
    }
}

inline sim::TerrainMap build_terrain_map(const LoadedMap& map) {
    sim::TerrainMap t;
    const sim::NavGrid& nav = map.nav;
    if (!nav.configured()) return t;
    t.configure(nav.width(), nav.height(), sim::Fixed::from_int(1), sim::Fixed(), sim::Fixed());
    for (int y = 0; y < nav.height(); ++y)
        for (int x = 0; x < nav.width(); ++x) {
            t.set_level(x, y, nav.level_cell(x, y));
            const bool ramp = nav.is_ramp_cell(x, y);
            if (ramp) t.set_flag(x, y, sim::kRamp);
            if (nav.is_blocked_cell(x, y)) {
                t.clear_flag(x, y, sim::kPathable);
                const std::size_t i = std::size_t(y) * std::size_t(nav.width()) + std::size_t(x);
                const bool body_nav = i < map.body_nav_block.size() && map.body_nav_block[i];
                const bool body_place = i < map.body_place_block.size() && map.body_place_block[i];
                if (!body_nav || body_place) t.clear_flag(x, y, sim::kBuildable);
            } else if (ramp) {
                t.clear_flag(x, y, sim::kBuildable);
            }
        }
    return t;
}

inline float map_vertex_height(const LoadedMap& map, std::uint32_t col, std::uint32_t row) {
    if (!map.sync_height.empty()) return map.sync_height.world_at(col, row);
    const HeightMap& hm = map.heightmap;
    const std::vector<std::uint8_t>& raw = map.heightmap_raw;
    if (hm.width == 0 || hm.height == 0 || hm.stride < 2) return 0.0f;
    if (col >= hm.width) col = hm.width - 1;
    if (row >= hm.height) row = hm.height - 1;
    const std::size_t rec = hm.data_offset + (std::size_t(row) * hm.width + col) * hm.stride;
    if (rec + 1 >= raw.size()) return 0.0f;
    const float scale = map.quantize.found ? float(map.quantize.quantize_scale) : 1.0f / 512.0f;
    const float bias = (map.quantize.found ? float(map.quantize.quantize_bias) : 0.0f) +
                       (map.quantize.found ? float(map.quantize.standard_height) : 0.0f);
    const std::uint16_t h = std::uint16_t(raw[rec]) | std::uint16_t(raw[rec + 1]) << 8;
    return float(h) * scale - bias;
}

inline float map_sculpt_height(const LoadedMap& map, std::uint32_t col, std::uint32_t row) {
    const HeightMap& hm = map.heightmap;
    const std::vector<std::uint8_t>& raw = map.heightmap_raw;
    if (hm.width == 0 || hm.height == 0 || hm.stride < 2) return 0.0f;
    if (col >= hm.width) col = hm.width - 1;
    if (row >= hm.height) row = hm.height - 1;
    const std::size_t rec = hm.data_offset + (std::size_t(row) * hm.width + col) * hm.stride;
    if (rec + 1 >= raw.size()) return 0.0f;
    const float scale = map.quantize.found ? float(map.quantize.quantize_scale) : 1.0f / 512.0f;
    const float bias = map.quantize.found ? float(map.quantize.quantize_bias) : 0.0f;
    const std::uint16_t h = std::uint16_t(raw[rec]) | std::uint16_t(raw[rec + 1]) << 8;
    return float(h) * scale - bias;
}

inline float map_cell_height(const LoadedMap& map, std::uint32_t col, std::uint32_t row) {
    return 0.25f * (map_vertex_height(map, col, row) + map_vertex_height(map, col + 1, row) +
                    map_vertex_height(map, col, row + 1) + map_vertex_height(map, col + 1, row + 1));
}

inline bool measure_lowest_tier_height(const LoadedMap& map, float& lowest_height, float& tier_step) {
    tier_step = 2.0f;
    const CliffMap& cm = map.cliff;
    if (cm.empty()) return false;
    std::uint8_t maxL = 0;
    for (std::uint8_t v : cm.levels) maxL = v > maxL ? v : maxL;
    if (maxL == 0) return false;
    std::vector<double> sum(std::size_t(maxL) + 1, 0.0);
    std::vector<std::uint64_t> cnt(std::size_t(maxL) + 1, 0);
    for (std::uint32_t y = 0; y < cm.height; ++y)
        for (std::uint32_t x = 0; x < cm.width; ++x) {
            const std::uint8_t L = cm.levels[std::size_t(y) * cm.width + x];
            if (L == 0) continue;
            sum[L] += double(map_cell_height(map, x, y));
            cnt[L] += 1;
        }
    bool have = false;
    std::vector<float> diffs;
    float last = 0.0f;
    for (std::size_t L = 1; L <= maxL; ++L) {
        if (!cnt[L]) continue;
        const float h = float(sum[L] / double(cnt[L]));
        if (!have) { lowest_height = h; have = true; }
        else diffs.push_back(h - last);
        last = h;
    }
    if (!have) return false;
    if (!diffs.empty()) {
        std::sort(diffs.begin(), diffs.end());
        const std::size_t n = diffs.size();
        const float md = (n % 2) ? diffs[n / 2] : 0.5f * (diffs[n / 2 - 1] + diffs[n / 2]);
        if (md > 1e-3f) tier_step = md;
    }
    return true;
}

inline std::vector<float> tier_heights(const LoadedMap& map) {
    if (map.cliff.empty()) return {};

    auto height_at = [&](std::uint32_t col, std::uint32_t row) -> float {
        return map_vertex_height(map, col, row);
    };

    const CliffMap& cm = map.cliff;
    std::uint8_t maxL = 0;
    for (std::uint8_t v : cm.levels) maxL = v > maxL ? v : maxL;
    std::vector<double> sum(std::size_t(maxL) + 1, 0.0);
    std::vector<std::uint64_t> cnt(std::size_t(maxL) + 1, 0);
    std::vector<std::map<int, std::uint64_t>> hist(std::size_t(maxL) + 1);
    for (std::uint32_t y = 0; y < cm.height; ++y)
        for (std::uint32_t x = 0; x < cm.width; ++x) {
            const std::uint8_t L = cm.levels[std::size_t(y) * cm.width + x];
            const float z = height_at(x, y);
            sum[L] += map_cell_height(map, x, y);
            cnt[L] += 1;
            ++hist[L][int(z * 256.0f + (z < 0.0f ? -0.5f : 0.5f))];
        }
    std::vector<float> level_height(std::size_t(maxL) + 1, 0.0f);
    constexpr double kPlateauModeShare = 0.30;
    for (std::size_t L = 0; L <= maxL; ++L) {
        if (!cnt[L]) continue;
        const double mean = sum[L] / double(cnt[L]);
        int mode_q = 0;
        std::uint64_t best = 0;
        for (const auto& kv : hist[L])
            if (kv.second > best) { best = kv.second; mode_q = kv.first; }
        const double share = double(best) / double(cnt[L]);
        level_height[L] = share >= kPlateauModeShare ? float(double(mode_q) / 256.0) : float(mean);
    }
    float step = 2.0f;
    {
        float lowest_unused = 0.0f;
        float measured = 2.0f;
        if (measure_lowest_tier_height(map, lowest_unused, measured) && measured > 1e-3f)
            step = measured;
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
    return level_height;
}

inline float air_floor_z(const LoadedMap& map) {
    const CliffMap& cm = map.cliff;
    if (cm.empty()) return 0.0f;
    const std::vector<float> th = tier_heights(map);
    float floor = 1e30f;
    std::vector<bool> seen(th.size(), false);
    for (std::uint8_t L : cm.levels)
        if (L > 0 && std::size_t(L) < th.size() && !seen[L]) {
            seen[L] = true;
            floor = std::min(floor, th[L]);
        }
    return floor < 1e29f ? floor : 0.0f;
}

inline float tier_height_at(const LoadedMap& map, unsigned level) {
    const std::vector<float> th = tier_heights(map);
    if (th.empty()) return float(level) * 2.0f;
    return th[level < th.size() ? level : th.size() - 1];
}

inline std::vector<std::uint8_t> ramp_mask(const CliffMap& cm) {
    std::vector<std::uint8_t> ramp(std::size_t(cm.width) * cm.height, 0);
    if (cm.empty()) return ramp;
    if (cm.raw.size() == std::size_t(cm.width) * cm.height) {
        for (std::size_t i = 0; i < ramp.size(); ++i) ramp[i] = (cm.raw[i] & 63) ? 1 : 0;
        return ramp;
    }
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
    return ramp;
}

inline std::vector<float> tier_relief(const LoadedMap& map);

inline std::vector<float> bake_ground_heights(const LoadedMap& map, std::uint32_t& out_w,
                                              std::uint32_t& out_h) {
    out_w = map.cliff.width;
    out_h = map.cliff.height;
    std::vector<float> out;
    if (map.cliff.empty() || out_w == 0 || out_h == 0) {
        out_w = out_h = 0;
        return out;
    }
    const std::vector<float> level_height = tier_heights(map);
    const std::vector<std::uint8_t> ramp = ramp_mask(map.cliff);
    const std::vector<float> relief = tier_relief(map);
    out.resize(std::size_t(out_w) * out_h, 0.0f);
    for (std::uint32_t y = 0; y < out_h; ++y)
        for (std::uint32_t x = 0; x < out_w; ++x)
            out[std::size_t(y) * out_w + x] =
                graded_height_at(map.cliff, level_height, &ramp, float(x) + 0.5f, float(y) + 0.5f, &relief);
    return out;
}

inline std::vector<float> tier_relief_unclamped(const LoadedMap& map);
inline std::vector<float> tier_relief(const LoadedMap& map);

inline std::vector<float> tier_relief_unclamped(const LoadedMap& map) {
    std::vector<float> out;
    if (map.cliff.empty()) return out;
    const std::vector<float> level_height = tier_heights(map);
    if (level_height.empty()) return out;
    const CliffMap& cm = map.cliff;
    const bool have_raw = cm.raw.size() == std::size_t(cm.width) * cm.height;
    const std::size_t n = std::size_t(cm.width) * cm.height;
    std::vector<float> off(n, 0.0f);
    std::vector<std::uint8_t> lvl(n, 0);
    std::vector<double> sum(level_height.size(), 0.0);
    std::vector<std::uint64_t> cnt(level_height.size(), 0);
    std::vector<std::map<int, std::uint64_t>> hist(level_height.size());
    for (std::uint32_t y = 0; y < cm.height; ++y)
        for (std::uint32_t x = 0; x < cm.width; ++x) {
            const std::size_t k = std::size_t(y) * cm.width + x;
            std::uint8_t L = cm.levels[k];
            if (L >= level_height.size()) L = std::uint8_t(level_height.size() - 1);
            lvl[k] = L;
            off[k] = map_cell_height(map, x, y) - level_height[L];
            sum[L] += off[k];
            cnt[L] += 1;
            ++hist[L][int(off[k] * 256.0f + (off[k] < 0.0f ? -0.5f : 0.5f))];
        }
    std::vector<float> shift(level_height.size(), 0.0f);
    for (std::size_t L = 0; L < level_height.size(); ++L) {
        if (!cnt[L]) continue;
        int mode_q = 0;
        std::uint64_t best = 0;
        for (const auto& kv : hist[L])
            if (kv.second > best) { best = kv.second; mode_q = kv.first; }
        shift[L] = double(best) / double(cnt[L]) >= 0.30 ? float(double(mode_q) / 256.0) : float(sum[L] / double(cnt[L]));
    }
    out.assign(n, 0.0f);
    for (std::size_t k = 0; k < n; ++k) {
        if (have_raw && (cm.raw[k] & 63)) continue;
        out[k] = off[k] - shift[lvl[k]];
    }
    return out;
}

inline std::vector<float> tier_relief(const LoadedMap& map) {
    std::vector<float> out = tier_relief_unclamped(map);
    if (out.empty()) return out;
    const std::vector<float> level_height = tier_heights(map);
    float step = 0.0f;
    for (std::size_t L = 1; L < level_height.size(); ++L) {
        const float d = level_height[L] - level_height[L - 1];
        if (d > 1e-3f && (step <= 0.0f || d < step)) step = d;
    }
    const float band = step > 0.0f ? 0.45f * step : 0.45f;
    const CliffMap& cm = map.cliff;
    const bool have_raw = cm.raw.size() == std::size_t(cm.width) * cm.height;
    constexpr int kNear = 3;
    for (std::uint32_t y = 0; y < cm.height; ++y)
        for (std::uint32_t x = 0; x < cm.width; ++x) {
            const std::size_t k = std::size_t(y) * cm.width + x;
            bool near = false;
            for (int dy = -kNear; dy <= kNear && !near; ++dy)
                for (int dx = -kNear; dx <= kNear && !near; ++dx) {
                    const int nx = int(x) + dx, ny = int(y) + dy;
                    if (nx < 0 || ny < 0 || nx >= int(cm.width) || ny >= int(cm.height)) continue;
                    if (cm.levels[std::size_t(ny) * cm.width + nx] != cm.levels[k]) near = true;
                }
            if (near) out[k] = out[k] > band ? band : (out[k] < -band ? -band : out[k]);
        }
    for (std::uint32_t y = 0; y < cm.height; ++y)
        for (std::uint32_t x = 0; x < cm.width; ++x) {
            const std::size_t k = std::size_t(y) * cm.width + x;
            const std::uint8_t L = cm.levels[k];
            bool edge = false;
            for (int dy = -1; dy <= 1 && !edge; ++dy)
                for (int dx = -1; dx <= 1 && !edge; ++dx) {
                    const int nx = int(x) + dx, ny = int(y) + dy;
                    if (nx < 0 || ny < 0 || nx >= int(cm.width) || ny >= int(cm.height)) continue;
                    const std::size_t q = std::size_t(ny) * cm.width + nx;
                    if (cm.levels[q] != L || (have_raw && (cm.raw[q] & 63))) edge = true;
                }
            if (edge) out[k] = 0.0f;
        }
    return out;
}

inline float engine_z_offset(const LoadedMap& map);
inline float vertex_ground_z(const LoadedMap& map, const std::vector<float>& th, float off, std::uint32_t c,
                             std::uint32_t r) {
    const std::uint32_t W = map.cliff.width, H = map.cliff.height;
    int lo = 1 << 30, hi = -1;
    for (int dy = -1; dy <= 0; ++dy)
        for (int dx = -1; dx <= 0; ++dx) {
            const int x = int(c) + dx, y = int(r) + dy;
            if (x < 0 || y < 0 || x >= int(W) || y >= int(H)) continue;
            const int L = map.cliff.levels[std::size_t(y) * W + std::size_t(x)];
            lo = std::min(lo, L); hi = std::max(hi, L);
        }
    (void)th; (void)lo; (void)hi;
    return map_vertex_height(map, c, r) - off;
}

inline float vertex_ground_bilinear(const LoadedMap& map, const std::vector<float>& th, float off, float x, float y) {
    const float W = float(map.cliff.width), H = float(map.cliff.height);
    x = std::min(std::max(x, 0.0f), W);
    y = std::min(std::max(y, 0.0f), H);
    const std::uint32_t c = std::min(std::uint32_t(x), map.cliff.width ? map.cliff.width - 1 : 0u);
    const std::uint32_t r = std::min(std::uint32_t(y), map.cliff.height ? map.cliff.height - 1 : 0u);
    const float fx = x - float(c), fy = y - float(r);
    const float z00 = vertex_ground_z(map, th, off, c, r), z10 = vertex_ground_z(map, th, off, c + 1, r);
    const float z01 = vertex_ground_z(map, th, off, c, r + 1), z11 = vertex_ground_z(map, th, off, c + 1, r + 1);
    return (z00 * (1.0f - fx) + z10 * fx) * (1.0f - fy) + (z01 * (1.0f - fx) + z11 * fx) * fy;
}

inline bool vertex_cell_wall(const LoadedMap& map, const std::vector<float>& th, float off, std::uint32_t c,
                             std::uint32_t r, float z[4]) {
    z[0] = vertex_ground_z(map, th, off, c, r);
    z[1] = vertex_ground_z(map, th, off, c + 1, r);
    z[2] = vertex_ground_z(map, th, off, c, r + 1);
    z[3] = vertex_ground_z(map, th, off, c + 1, r + 1);
    const float mn = std::min(std::min(z[0], z[1]), std::min(z[2], z[3]));
    const float mx = std::max(std::max(z[0], z[1]), std::max(z[2], z[3]));
    static const bool no_walls = std::getenv("OMC_NO_WALL_CELLS") != nullptr;
    if (no_walls || mx - mn <= 0.5f || th.empty()) return false;
    const std::uint32_t W = map.cliff.width, H = map.cliff.height;
    int lo = 1 << 30, hi = -1;
    for (int dy = -1; dy <= 1; ++dy)
        for (int dx = -1; dx <= 1; ++dx) {
            const int x = int(c) + dx, y = int(r) + dy;
            if (x < 0 || y < 0 || x >= int(W) || y >= int(H)) continue;
            const std::size_t k = std::size_t(y) * W + std::size_t(x);
            if (map.cliff.raw.size() == std::size_t(W) * H && (map.cliff.raw[k] & 63)) return false;
            const int L = map.cliff.levels[k];
            lo = std::min(lo, L); hi = std::max(hi, L);
        }
    if (hi <= lo) return false;
    const float step = th[std::size_t(std::min(hi, int(th.size()) - 1))] - th[std::size_t(std::min(lo, int(th.size()) - 1))];
    if (mx - mn <= 0.5f * step) return false;
    for (int i = 0; i < 4; ++i) z[i] = std::min(z[i], mn + 0.3f);
    return true;
}

inline std::vector<float> bake_ground_heights_vertex(const LoadedMap& map, std::uint32_t& out_w, std::uint32_t& out_h) {
    out_w = map.cliff.width;
    out_h = map.cliff.height;
    std::vector<float> out;
    if (map.cliff.empty()) { out_w = out_h = 0; return out; }
    const std::vector<float> th = tier_heights(map);
    const float off = engine_z_offset(map);
    out.resize(std::size_t(out_w) * out_h);
    for (std::uint32_t y = 0; y < out_h; ++y)
        for (std::uint32_t x = 0; x < out_w; ++x) {
            float z[4];
            vertex_cell_wall(map, th, off, x, y, z);
            out[std::size_t(y) * out_w + x] = 0.25f * (z[0] + z[1] + z[2] + z[3]);
        }
    return out;
}

inline Model build_map_mesh_vertex(const LoadedMap& map, float cell = 1.0f, const std::vector<std::uint8_t>* skip = nullptr) {
    Model m;
    if (map.cliff.empty()) return m;
    const std::uint32_t W = map.cliff.width, H = map.cliff.height, VW = W + 1;
    const float off = engine_z_offset(map);
    const std::vector<float> th = tier_heights(map);
    const std::uint32_t rows_per_band = std::max<std::uint32_t>(1, 60000u / VW - 1);
    struct WallCell { std::uint32_t c, r; float z[4]; };
    std::vector<WallCell> walls;
    std::vector<std::pair<std::uint32_t, std::uint32_t>> under;
    auto push = [&](std::uint32_t c, std::uint32_t r) {
        m.positions.push_back(float(c) * cell);
        m.positions.push_back(float(r) * cell);
        const float z0 = vertex_ground_z(map, th, off, c, r);
        m.positions.push_back(z0);
        m.uvs.push_back(float(c) / float(W));
        m.uvs.push_back(float(r) / float(H));
        static const bool kSeamNormal = std::getenv("OMC_TERRAIN_SEAM_NORMAL") != nullptr;
        auto seam = [&](int vc, int vr) {
            if (kSeamNormal || map.cliff.levels.size() != std::size_t(W) * H) return false;
            int lo = 1 << 30, hi = -1;
            for (int dy = -1; dy <= 0; ++dy)
                for (int dx = -1; dx <= 0; ++dx) {
                    const int x = vc + dx, y = vr + dy;
                    if (x < 0 || y < 0 || x >= int(W) || y >= int(H)) continue;
                    const std::size_t k = std::size_t(y) * W + std::size_t(x);
                    if (map.cliff.raw.size() == std::size_t(W) * H && (map.cliff.raw[k] & 63)) return false;
                    const int L = map.cliff.levels[k];
                    lo = std::min(lo, L); hi = std::max(hi, L);
                }
            return hi > lo;
        };
        const bool own_seam = seam(int(c), int(r));
        auto slope = [&](int dc, int dr) {
            float s = 0.0f;
            int n = 0;
            if (own_seam) return 0.0f;
            for (int sgn = -1; sgn <= 1; sgn += 2) {
                const int nc = int(c) + sgn * dc, nr = int(r) + sgn * dr;
                if (nc < 0 || nr < 0 || nc > int(W) || nr > int(H)) continue;
                if (seam(nc, nr)) continue;
                const float dz = vertex_ground_z(map, th, off, std::uint32_t(nc), std::uint32_t(nr)) - z0;
                if (std::fabs(dz) > 1.0f) continue;
                s += float(sgn) * dz;
                ++n;
            }
            return n ? s / (float(n) * cell) : 0.0f;
        };
        static const float kNormalGain = std::getenv("OMC_TERRAIN_NORMAL_GAIN") ? float(std::atof(std::getenv("OMC_TERRAIN_NORMAL_GAIN"))) : 1.0f;
        const float gx = slope(1, 0) * kNormalGain, gy = slope(0, 1) * kNormalGain, gl = std::sqrt(gx * gx + gy * gy + 1.0f);
        m.normals.insert(m.normals.end(), {-gx / gl, -gy / gl, 1.0f / gl});
    };
    for (std::uint32_t r0 = 0; r0 < H; r0 += rows_per_band) {
        const std::uint32_t r1 = std::min(H, r0 + rows_per_band);
        M3Region rg;
        rg.first_vertex = std::uint32_t(m.positions.size() / 3);
        rg.first_index = std::uint32_t(m.indices.size());
        for (std::uint32_t r = r0; r <= r1; ++r)
            for (std::uint32_t c = 0; c <= W; ++c) push(c, r);
        for (std::uint32_t r = 0; r < r1 - r0; ++r)
            for (std::uint32_t c = 0; c < W; ++c) {
                float z[4];
                if (skip && skip->size() == std::size_t(W) * H && (*skip)[std::size_t(r0 + r) * W + c]) { under.push_back({c, r0 + r}); continue; }
                if (vertex_cell_wall(map, th, off, c, r0 + r, z)) { walls.push_back({c, r0 + r, {z[0], z[1], z[2], z[3]}}); continue; }
                const std::uint16_t a = std::uint16_t(r * VW + c), b = std::uint16_t(a + 1),
                                    d = std::uint16_t(a + VW), e = std::uint16_t(d + 1);
                m.indices.insert(m.indices.end(), {a, d, b, b, d, e});
            }
        rg.num_vertices = std::uint32_t(m.positions.size() / 3) - rg.first_vertex;
        rg.num_indices = std::uint32_t(m.indices.size()) - rg.first_index;
        m.regions.push_back(rg);
        m.region_material.push_back(-1);
    }
    for (std::size_t w0 = 0; w0 < walls.size(); w0 += 3000) {
        const std::size_t w1 = std::min(walls.size(), w0 + 3000);
        M3Region rg;
        rg.first_vertex = std::uint32_t(m.positions.size() / 3);
        rg.first_index = std::uint32_t(m.indices.size());
        auto tri = [&](const float p[3][3], bool both) {
            const float nz = (p[1][0] - p[0][0]) * (p[2][1] - p[0][1]) - (p[1][1] - p[0][1]) * (p[2][0] - p[0][0]);
            const std::uint32_t nv = std::uint32_t(m.positions.size() / 3) - rg.first_vertex;
            if (nv + 3 > 60000u) return;
            for (int i = 0; i < 3; ++i) {
                m.positions.insert(m.positions.end(), {p[i][0] * cell, p[i][1] * cell, p[i][2]});
                m.normals.insert(m.normals.end(), {0.0f, 0.0f, 1.0f});
                m.uvs.push_back(p[i][0] / float(W));
                m.uvs.push_back(p[i][1] / float(H));
            }
            const std::uint16_t a = std::uint16_t(nv), b = std::uint16_t(nv + 1), c = std::uint16_t(nv + 2);
            if (nz >= 0.0f || both) m.indices.insert(m.indices.end(), {a, b, c});
            if (nz < 0.0f || both) m.indices.insert(m.indices.end(), {a, c, b});
        };
        for (std::size_t k = w0; k < w1; ++k) {
            const WallCell& wc = walls[k];
            float raw[4], x[4], y[4];
            int nh = 0;
            bool high[4];
            for (int i = 0; i < 4; ++i) {
                x[i] = float(wc.c + std::uint32_t(i & 1));
                y[i] = float(wc.r + std::uint32_t(i >> 1));
                raw[i] = vertex_ground_z(map, th, off, wc.c + std::uint32_t(i & 1), wc.r + std::uint32_t(i >> 1));
            }
            const float mn = std::min(std::min(raw[0], raw[1]), std::min(raw[2], raw[3]));
            const float mx = std::max(std::max(raw[0], raw[1]), std::max(raw[2], raw[3]));
            for (int i = 0; i < 4; ++i) { high[i] = raw[i] > 0.5f * (mn + mx); nh += high[i] ? 1 : 0; }
            auto P = [&](float p[3], int i, bool flat) { p[0] = x[i]; p[1] = y[i]; p[2] = flat ? wc.z[i] : raw[i]; };
            auto wall = [&](int i, int j, float dx, float dy) {
                const float dl = std::sqrt(dx * dx + dy * dy);
                if (dl > 1e-5f) { dx /= dl; dy /= dl; }
                float q[3][3], fi[3], fj[3];
                P(fi, i, true); P(fj, j, true);
                fi[0] += dx; fi[1] += dy; fj[0] += dx; fj[1] += dy;
                const float lowz = std::min(fi[2], fj[2]);
                fi[2] = fj[2] = lowz;
                P(q[0], i, false); P(q[1], j, false); std::copy(fi, fi + 3, q[2]); tri(q, true);
                P(q[0], j, false); std::copy(fj, fj + 3, q[1]); std::copy(fi, fi + 3, q[2]); tri(q, true);
                P(q[0], i, true); P(q[1], j, true); std::copy(fi, fi + 3, q[2]); tri(q, true);
                P(q[0], j, true); std::copy(fj, fj + 3, q[1]); std::copy(fi, fi + 3, q[2]); tri(q, true);
            };
            float q[3][3];
            static const int kAdj[4][2] = {{1, 2}, {0, 3}, {0, 3}, {1, 2}};
            if (nh == 3) {
                int l = 0;
                for (int i = 0; i < 4; ++i) if (!high[i]) l = i;
                const int n1 = kAdj[l][0], n2 = kAdj[l][1], o = 3 - l;
                P(q[0], n1, false); P(q[1], n2, false); P(q[2], o, false); tri(q, false);
                P(q[0], n1, true); P(q[1], n2, true); P(q[2], l, true); tri(q, false);
                wall(n1, n2, x[o] - x[l], y[o] - y[l]);
            } else {
                P(q[0], 0, true); P(q[1], 1, true); P(q[2], 2, true); tri(q, false);
                P(q[0], 1, true); P(q[1], 3, true); P(q[2], 2, true); tri(q, false);
                static const int kEdge[4][2] = {{0, 1}, {1, 3}, {3, 2}, {2, 0}};
                if (nh == 2)
                    for (const auto& e : kEdge)
                        if (high[e[0]] && high[e[1]])
                            wall(e[0], e[1], 0.5f * (x[e[0]] + x[e[1]]) - (float(wc.c) + 0.5f), 0.5f * (y[e[0]] + y[e[1]]) - (float(wc.r) + 0.5f));
            }
        }
        rg.num_vertices = std::uint32_t(m.positions.size() / 3) - rg.first_vertex;
        rg.num_indices = std::uint32_t(m.indices.size()) - rg.first_index;
        m.regions.push_back(rg);
        m.region_material.push_back(-1);
    }
    for (std::size_t u0 = 0; u0 < under.size(); u0 += 10000) {
        const std::size_t u1 = std::min(under.size(), u0 + 10000);
        M3Region rg;
        rg.first_vertex = std::uint32_t(m.positions.size() / 3);
        rg.first_index = std::uint32_t(m.indices.size());
        for (std::size_t k = u0; k < u1; ++k) {
            const std::uint32_t c = under[k].first, r = under[k].second;
            float lowz = 1e30f;
            for (int i = 0; i < 4; ++i)
                lowz = std::min(lowz, vertex_ground_z(map, th, off, c + std::uint32_t(i & 1), r + std::uint32_t(i >> 1)));
            const std::uint16_t a = std::uint16_t((k - u0) * 4);
            for (int i = 0; i < 4; ++i) {
                const float x = float(c + std::uint32_t(i & 1)), y = float(r + std::uint32_t(i >> 1));
                m.positions.insert(m.positions.end(), {x * cell, y * cell, lowz - 0.15f});
                m.normals.insert(m.normals.end(), {0.0f, 0.0f, 1.0f});
                m.uvs.push_back(x / float(W));
                m.uvs.push_back(y / float(H));
            }
            m.indices.insert(m.indices.end(), {a, std::uint16_t(a + 2), std::uint16_t(a + 1), std::uint16_t(a + 1), std::uint16_t(a + 2), std::uint16_t(a + 3)});
        }
        rg.num_vertices = std::uint32_t(m.positions.size() / 3) - rg.first_vertex;
        rg.num_indices = std::uint32_t(m.indices.size()) - rg.first_index;
        m.regions.push_back(rg);
        m.region_material.push_back(-1);
    }
    m.vertex_count = std::uint32_t(m.positions.size() / 3);
    for (int a = 0; a < 3; ++a) { m.bounds_min[a] = 1e30f; m.bounds_max[a] = -1e30f; }
    for (std::uint32_t i = 0; i < m.vertex_count; ++i)
        for (int a = 0; a < 3; ++a) {
            m.bounds_min[a] = std::min(m.bounds_min[a], m.positions[i * 3 + a]);
            m.bounds_max[a] = std::max(m.bounds_max[a], m.positions[i * 3 + a]);
        }
    return m;
}

inline Model build_map_mesh_stepped(const LoadedMap& map, float cell = 1.0f,
                                    std::uint32_t max_dim = 128, bool with_walls = true) {
    if (map.cliff.empty()) {
        Model smooth = build_map_mesh(map, cell, max_dim);
        fit_mesh_to_playable(smooth, map);
        return smooth;
    }
    const CliffMap& cm = map.cliff;
    const std::vector<float> level_height = tier_heights(map);

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

    const std::vector<float> relief = tier_relief(map);
    Model mesh = build_terrain_mesh_stepped(map.cliff, level_height, cell, &ramp, with_walls, &relief);
    fit_mesh_to_playable(mesh, map);
    return mesh;
}

inline float engine_z_offset(const LoadedMap& map) {
    if (map.cliff.empty()) return 0.0f;
    const std::vector<float> th = tier_heights(map);
    const CliffMap& cm = map.cliff;
    std::vector<std::uint64_t> cnt(th.size(), 0);
    for (std::uint8_t L : cm.levels) if (L < th.size()) ++cnt[L];
    std::size_t best = 0;
    for (std::size_t L = 1; L < cnt.size(); ++L) if (cnt[L] > cnt[best]) best = L;
    std::map<int, std::uint64_t> hist;
    for (std::uint32_t y = 0; y < cm.height; ++y)
        for (std::uint32_t x = 0; x < cm.width; ++x)
            if (cm.levels[std::size_t(y) * cm.width + x] == best)
                ++hist[int(map_cell_height(map, x, y) * 16.0f + 0.5f)];
    int mode = 0;
    std::uint64_t n = 0;
    for (const auto& kv : hist) if (kv.second > n) { n = kv.second; mode = kv.first; }
    return float(mode) / 16.0f - th[best];
}

inline Model build_map_water(const LoadedMap& map, float cell = 1.0f, float lift = 0.5f,
                             const std::unordered_map<std::string, float>* type_heights = nullptr) {
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
        if (type_heights) {
            auto it = type_heights->find(p.type);
            if (it != type_heights->end()) { level.push_back(it->second - engine_z_offset(map)); continue; }
        }
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

inline void append_map_water(Model& ground, const LoadedMap& map, float cell = 1.0f,
                             const std::unordered_map<std::string, float>* type_heights = nullptr) {
    Model w = build_map_water(map, cell, 0.5f, type_heights);
    if (w.regions.empty()) return;
    const std::uint32_t vbase = std::uint32_t(ground.positions.size() / 3);
    const std::uint32_t ibase = std::uint32_t(ground.indices.size());
    ground.positions.insert(ground.positions.end(), w.positions.begin(), w.positions.end());
    ground.uvs.insert(ground.uvs.end(), w.uvs.begin(), w.uvs.end());
    if (ground.normals.size() == std::size_t(vbase) * 3)
        for (std::size_t i = 0; i < w.positions.size() / 3; ++i) ground.normals.insert(ground.normals.end(), {0.0f, 0.0f, 1.0f});
    ground.indices.reserve(ground.indices.size() + w.indices.size());
    for (std::uint16_t idx : w.indices) ground.indices.push_back(idx);
    for (std::size_t r = 0; r < w.regions.size(); ++r) {
        M3Region rg = w.regions[r];
        rg.first_vertex += vbase;
        rg.first_index += ibase;
        ground.regions.push_back(rg);
        ground.region_material.push_back(kUntexturedRegion);
    }
    ground.vertex_count = std::uint32_t(ground.positions.size() / 3);
}

}
