#include "sc2/formats/sc2map.hpp"

#include <stdexcept>
#include <utility>

#include "sc2/formats/map_info.hpp"

namespace sc2::formats {

MapInfo MapInfo::parse(const std::vector<uint8_t>& data) {
    const MapInfoHeader h = parse_map_info(data);
    if (h.width == 0 || h.height == 0 || h.width > 4096 || h.height > 4096)
        throw std::runtime_error("sc2map: MapInfo implausible dimensions");
    if (h.tileset.empty()) throw std::runtime_error("sc2map: MapInfo tileset did not resolve");

    MapInfo mi;
    mi.version = h.version;
    mi.width = h.width;
    mi.height = h.height;
    mi.fog_type = h.fog_type;
    mi.tileset = h.tileset;
    if (h.has_camera_bounds) {
        mi.camera_left = h.camera_left;
        mi.camera_bottom = h.camera_bottom;
        mi.camera_right = h.camera_right;
        mi.camera_top = h.camera_top;
    }
    return mi;
}

Sc2Map Sc2Map::open(const std::vector<uint8_t>& bytes) {
    Sc2Map m;
    m.mpq_ = Mpq::open(bytes);
    if (!m.mpq_.contains("MapInfo"))
        throw std::runtime_error("sc2map: archive has no MapInfo component");
    m.info_ = MapInfo::parse(m.mpq_.read_file("MapInfo"));
    return m;
}

Sc2Map Sc2Map::open(std::vector<uint8_t>&& bytes) {
    Sc2Map m;
    m.mpq_ = Mpq::open(std::move(bytes));
    if (!m.mpq_.contains("MapInfo"))
        throw std::runtime_error("sc2map: archive has no MapInfo component");
    m.info_ = MapInfo::parse(m.mpq_.read_file("MapInfo"));
    return m;
}

}
