#include "sc2/campaign/mission_load.hpp"

#include <exception>
#include <stdexcept>
#include <utility>

#include "sc2/casc/storage.hpp"
#include "sc2/formats/map_info.hpp"
#include "sc2/formats/placed_objects.hpp"
#include "sc2/formats/terrain.hpp"
#include "sc2/formats/terrain_grid.hpp"

namespace sc2::campaign {

std::vector<std::string> liberty_campaign_roots() {
    return {
        "campaigns/liberty.sc2campaign/base.sc2data",
        "campaigns/libertystory.sc2campaign/base.sc2data",
    };
}

static std::string trim_slash(std::string p) {
    if (!p.empty() && p.back() == '/') p.pop_back();
    return p;
}

std::string mission_local_data_root(const std::string& map_prefix) {
    if (map_prefix.empty()) return {};
    return trim_slash(map_prefix) + "/base.sc2data";
}

std::vector<std::uint8_t> load_mission_trigger_chunk(const casc::Storage& store,
                                                     const std::string& map_prefix) {
    return store.read_by_name(trim_slash(map_prefix) + "/Triggers");
}

formats::LoadedMap load_mission_map(const casc::Storage& store, const std::string& map_prefix) {
    const std::string base = trim_slash(map_prefix) + "/";

    auto read_chunk = [&](const char* name, bool& ok) -> std::vector<std::uint8_t> {
        try {
            ok = true;
            return store.read_by_name(base + name);
        } catch (const std::exception&) {
            ok = false;
            return {};
        }
    };

    bool ok = false;
    std::vector<std::uint8_t> hb = read_chunk("t3HeightMap", ok);
    if (!ok)
        throw std::runtime_error("mission_load: no t3HeightMap under '" + base + "' (not a terrain map?)");

    formats::LoadedMap out;
    out.heightmap = formats::HeightMap::parse(hb);

    formats::TerrainQuantize q;
    std::vector<std::uint8_t> tx = read_chunk("t3Terrain.xml", ok);
    if (ok) {
        q = formats::parse_terrain_quantize(tx);
        out.tileset = formats::parse_terrain_tileset(tx);
        out.tileset_textures = formats::parse_terrain_textures(tx);
    }

    out.terrain = formats::build_terrain_grid(out.heightmap, hb, q);
    out.quantize = q;
    out.heightmap_raw = hb;
    out.width = out.terrain.width;
    out.height = out.terrain.height;
    out.blocked_cells = formats::terrain_grid_to_nav(out.terrain, out.nav);

    std::vector<std::uint8_t> clif = read_chunk("t3SyncCliffLevel", ok);
    if (ok) {
        try {
            out.cliff = formats::CliffMap::parse(clif);
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
            out.cliff = formats::CliffMap{};
            out.has_cliff_levels = false;
        }
    }

    std::vector<std::uint8_t> wtr = read_chunk("t3Water", ok);
    if (ok) {
        try {
            formats::WaterMap wm = formats::WaterMap::parse(wtr);
            if (!wm.empty()) out.water = std::move(wm);
        } catch (const std::exception&) {
            out.water = formats::WaterMap{};
        }
    }

    std::vector<std::uint8_t> objs = read_chunk("Objects", ok);
    if (ok) out.objects = formats::parse_placed_objects(objs);

    std::vector<std::uint8_t> mi = read_chunk("MapInfo", ok);
    if (ok) out.info = formats::parse_map_info(mi);

    std::vector<std::uint8_t> ti = read_chunk("t3SyncTextureInfo", ok);
    bool have_ti = ok;
    std::vector<std::uint8_t> tm = read_chunk("t3TextureMasks", ok);
    bool have_tm = ok;
    if (have_ti && have_tm) {
        try {
            out.texture_info = formats::TextureInfo::parse(ti);
            out.texture_mask = formats::TextureMask::parse(tm);
            out.has_splat = out.texture_mask.mask_w == out.texture_info.width * 8 &&
                            out.texture_mask.mask_h == out.texture_info.height * 8;
        } catch (const std::exception&) {
            out.texture_info = formats::TextureInfo{};
            out.texture_mask = formats::TextureMask{};
            out.has_splat = false;
        }
    }

    return out;
}

}
