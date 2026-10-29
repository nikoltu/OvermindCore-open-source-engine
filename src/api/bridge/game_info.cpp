#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include "s2clientprotocol/sc2api.pb.h"

#include "omc/api/bridge.hpp"
#include "omc/app/melee_game.hpp"
#include "omc/formats/map_archive.hpp"

namespace omc::api::bridge {

namespace {

template <typename Pred>
void pack_bitmap(SC2APIProtocol::ImageData* img, int w, int h, Pred set) {
    img->set_bits_per_pixel(1);
    img->mutable_size()->set_x(w);
    img->mutable_size()->set_y(h);
    const int stride = (w + 7) / 8;
    std::string data(std::size_t(stride) * std::size_t(h), '\0');
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            if (set(x, y))
                data[std::size_t(y) * std::size_t(stride) + std::size_t(x >> 3)] =
                    char(std::uint8_t(data[std::size_t(y) * std::size_t(stride) +
                                           std::size_t(x >> 3)]) |
                         std::uint8_t(0x80u >> (x & 7)));
    img->set_data(std::move(data));
}

constexpr float kHeightScale = 8.0f;
constexpr int kHeightZero = 127;
constexpr int kHeightMax = 255;
constexpr int kOutsidePlayable = 0;

constexpr int kMatchSides = 2;

int encode_height(float world_z) {
    const float scaled = world_z * kHeightScale;
    const int b = int(std::lround(scaled)) + kHeightZero;
    return std::min(kHeightMax, std::max(kOutsidePlayable, b));
}

}

void fill_game_info(Host& host, const View& view, SC2APIProtocol::ResponseGameInfo& out) {
    const formats::LoadedMap* map = host.map();
    out.set_map_name(host.map_name());
    for (const std::string& m : host.mod_names()) out.add_mod_names(m);
    if (!map) return;

    const int w = int(map->cell_width());
    const int h = int(map->cell_height());

    SC2APIProtocol::StartRaw* raw = out.mutable_start_raw();
    raw->mutable_map_size()->set_x(w);
    raw->mutable_map_size()->set_y(h);

    const sim::NavGrid& nav = map->nav;
    pack_bitmap(raw->mutable_pathing_grid(), w, h,
                [&](int x, int y) { return !nav.is_blocked_cell(x, y); });

    const sim::TerrainMap& placement = map->placement;
    pack_bitmap(raw->mutable_placement_grid(), w, h,
                [&](int x, int y) { return placement.is_buildable(x, y); });

    SC2APIProtocol::ImageData* height = raw->mutable_terrain_height();
    height->set_bits_per_pixel(8);
    height->mutable_size()->set_x(w);
    height->mutable_size()->set_y(h);
    std::string hdata(std::size_t(w) * std::size_t(h), char(kOutsidePlayable));
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            if (!map->info.in_playable_area(std::uint32_t(x), std::uint32_t(y))) continue;
            const float z = formats::map_vertex_height(*map, std::uint32_t(x), std::uint32_t(y));
            hdata[std::size_t(y) * std::size_t(w) + std::size_t(x)] = char(encode_height(z));
        }
    height->set_data(std::move(hdata));

    if (map->info.has_playable_area()) {
        SC2APIProtocol::RectangleI* area = raw->mutable_playable_area();
        area->mutable_p0()->set_x(int(map->info.playable_x0));
        area->mutable_p0()->set_y(int(map->info.playable_y0));
        area->mutable_p1()->set_x(int(map->info.playable_x1));
        area->mutable_p1()->set_y(int(map->info.playable_y1));
    }

    for (const std::pair<float, float>& p : map->objects.start_locations) {
        SC2APIProtocol::Point2D* sl = raw->add_start_locations();
        sl->set_x(p.first);
        sl->set_y(p.second);
    }

    for (int side = 0; side < kMatchSides; ++side) {
        SC2APIProtocol::PlayerInfo* pi = out.add_player_info();
        pi->set_player_id(std::uint32_t(side + 1));
        pi->set_type(side + 1 == view.player ? SC2APIProtocol::Participant
                                             : SC2APIProtocol::Computer);
    }
}

}
