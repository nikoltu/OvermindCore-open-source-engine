#pragma once

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
    std::vector<std::string> tileset_textures;
    TextureInfo texture_info;
    TextureMask texture_mask;
    bool has_splat = false;
    CliffMap cliff;
    bool has_cliff_levels = false;
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
        out.tileset_textures = parse_terrain_textures(tx);
    }

    out.terrain = build_terrain_grid(out.heightmap, hb, q);
    out.quantize = q;
    out.heightmap_raw = hb;
    out.width = out.terrain.width;
    out.height = out.terrain.height;
    out.blocked_cells = terrain_grid_to_nav(out.terrain, out.nav);

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
                out.nav.mark_ramps_from_levels();
                out.has_cliff_levels = out.nav.has_levels();
            }
        } catch (const std::exception&) {
            out.cliff = CliffMap{};
            out.has_cliff_levels = false;
        }
    }

    if (arc.contains("Objects")) out.objects = parse_placed_objects(arc.read_file("Objects"));
    if (arc.contains("MapInfo")) out.info = parse_map_info(arc.read_file("MapInfo"));

    if (arc.contains("t3SyncTextureInfo") && arc.contains("t3TextureMasks")) {
        try {
            out.texture_info = TextureInfo::parse(arc.read_file("t3SyncTextureInfo"));
            out.texture_mask = TextureMask::parse(arc.read_file("t3TextureMasks"));
            out.has_splat = out.texture_mask.mask_w == out.texture_info.width * 8 &&
                            out.texture_mask.mask_h == out.texture_info.height * 8;
        } catch (const std::exception&) {
            out.texture_info = TextureInfo{};
            out.texture_mask = TextureMask{};
            out.has_splat = false;
        }
    }
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
