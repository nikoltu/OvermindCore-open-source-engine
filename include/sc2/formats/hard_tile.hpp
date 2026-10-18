#pragma once
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace sc2::formats {

struct HardTilePoint {
    float pos[3] = {0, 0, 0};
    float normal[3] = {0, 0, 1};
    float tan_in[3] = {0, 0, 0};
    float tan_out[3] = {0, 0, 0};
    float tan_len = 0.0f;
    float width = 1.0f;
};

struct HardTile {
    std::string tile;
    std::vector<HardTilePoint> points;
};

inline std::vector<HardTile> parse_hard_tiles(const std::vector<std::uint8_t>& d) {
    std::vector<HardTile> out;
    if (d.size() < 0x1c || std::memcmp(d.data(), "HRDT", 4) != 0) return out;
    const auto u32 = [&](std::size_t o) {
        std::uint32_t v;
        std::memcpy(&v, &d[o], 4);
        return v;
    };
    std::size_t o = 0x18;
    const std::uint32_t roads = u32(o);
    o += 4;
    for (std::uint32_t r = 0; r < roads && r < 4096; ++r) {
        if (o + 4 > d.size()) break;
        const std::uint32_t n = u32(o);
        o += 4;
        if (n > 65536 || o + std::size_t(n) * 58 + 3 > d.size()) break;
        HardTile t;
        t.points.resize(n);
        for (std::uint32_t i = 0; i < n; ++i) {
            float f[14];
            std::memcpy(f, &d[o], 56);
            HardTilePoint& p = t.points[i];
            for (int k = 0; k < 3; ++k) {
                p.pos[k] = f[k];
                p.normal[k] = f[3 + k];
                p.tan_in[k] = f[6 + k];
                p.tan_out[k] = f[9 + k];
            }
            p.tan_len = f[12];
            p.width = f[13];
            o += 58;
        }
        o += 3;
        while (o < d.size() && d[o] != 0) t.tile.push_back(char(d[o++]));
        o += 1 + 4;
        if (o > d.size()) { out.push_back(std::move(t)); break; }
        out.push_back(std::move(t));
    }
    return out;
}

}
