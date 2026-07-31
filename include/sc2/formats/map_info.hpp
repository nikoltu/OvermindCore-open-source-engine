#pragma once

#include <cstdint>
#include <stdexcept>
#include <vector>

namespace sc2::formats {

struct MapInfoHeader {
    std::uint32_t version = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
};

inline MapInfoHeader parse_map_info(const std::vector<std::uint8_t>& b) {
    if (b.size() < 0x18)
        throw std::runtime_error("map_info: blob too small");
    const bool sig = b[0] == 'I' && b[1] == 'p' && b[2] == 'a' && b[3] == 'M';
    if (!sig)
        throw std::runtime_error("map_info: bad signature (not MapInfo)");

    auto u32 = [&](std::size_t off) -> std::uint32_t {
        return std::uint32_t(b[off]) | (std::uint32_t(b[off + 1]) << 8) |
               (std::uint32_t(b[off + 2]) << 16) | (std::uint32_t(b[off + 3]) << 24);
    };

    MapInfoHeader h;
    h.version = u32(0x04);
    h.width = u32(0x10);
    h.height = u32(0x14);
    return h;
}

}
