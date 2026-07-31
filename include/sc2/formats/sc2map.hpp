#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "sc2/formats/mpq.hpp"

namespace sc2::formats {

struct MapInfo {
    uint32_t version = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    std::string fog_type;
    std::string tileset;
    int32_t camera_left = 0;
    int32_t camera_bottom = 0;
    int32_t camera_right = 0;
    int32_t camera_top = 0;

    int32_t camera_width() const { return camera_right - camera_left; }
    int32_t camera_height() const { return camera_top - camera_bottom; }

    static MapInfo parse(const std::vector<uint8_t>& data);
};

class Sc2Map {
public:
    Sc2Map() = default;

    static Sc2Map open(const std::vector<uint8_t>& bytes);
    static Sc2Map open(std::vector<uint8_t>&& bytes);

    const MapInfo& info() const { return info_; }

    const Mpq& archive() const { return mpq_; }

private:
    Mpq mpq_;
    MapInfo info_;
};

}
