#include "sc2/formats/sc2map.hpp"

#include <stdexcept>
#include <utility>

#include "sc2/core/byte_reader.hpp"

namespace sc2::formats {

using core::ByteReader;

namespace {

std::string read_cstring(ByteReader& r) {
    std::string s;
    for (;;) {
        uint8_t c = r.u8();
        if (c == 0) break;
        s.push_back(char(c));
    }
    return s;
}

}

MapInfo MapInfo::parse(const std::vector<uint8_t>& data) {
    if (data.size() < 4 || data[0] != 'M' || data[1] != 'a' || data[2] != 'p' ||
        data[3] != 'I')
        throw std::runtime_error("sc2map: MapInfo bad magic");

    ByteReader r(data);
    r.seek(4);

    MapInfo mi;
    mi.version = r.u32le();
    if (mi.version >= 0x18) {
        r.u32le();
        r.u32le();
    }
    mi.width = r.u32le();
    mi.height = r.u32le();
    if (mi.width == 0 || mi.height == 0 || mi.width > 4096 || mi.height > 4096)
        throw std::runtime_error("sc2map: MapInfo implausible dimensions");

    if (r.u32le() == 2) read_cstring(r);
    if (r.u32le() == 2) read_cstring(r);

    mi.fog_type = read_cstring(r);
    mi.tileset = read_cstring(r);

    mi.camera_left = int32_t(r.u32le());
    mi.camera_bottom = int32_t(r.u32le());
    mi.camera_right = int32_t(r.u32le());
    mi.camera_top = int32_t(r.u32le());
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
