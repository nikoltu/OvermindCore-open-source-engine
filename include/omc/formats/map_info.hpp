#pragma once

#include <array>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace omc::formats {

inline constexpr std::uint32_t kPlayableSnap = 2;
inline constexpr std::uint32_t kPlayableEdgeMargin = 2;

inline constexpr std::size_t kMapInfoStringsOffset = 0x2A;

struct MapInfoHeader {
    std::uint32_t version = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;

    std::string fog_type;
    std::string tileset;

    std::int32_t camera_left = 0, camera_bottom = 0, camera_right = 0, camera_top = 0;
    bool has_camera_bounds = false;
    float base_height = 0.0f;
    bool has_base_height = false;
    std::array<std::int32_t, 16> player_color{{-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1}};
    bool has_players = false;

    std::uint32_t playable_x0 = 0, playable_y0 = 0, playable_x1 = 0, playable_y1 = 0;

    bool has_playable_area() const { return playable_x1 > playable_x0 && playable_y1 > playable_y0; }

    bool in_playable_area(std::uint32_t x, std::uint32_t y) const {
        if (!has_playable_area()) return true;
        return x >= playable_x0 && x < playable_x1 && y >= playable_y0 && y < playable_y1;
    }
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
    if (h.version < 0x18)
        throw std::runtime_error("map_info: version below 0x18 has no verified byte layout");
    h.width = u32(0x10);
    h.height = u32(0x14);

    constexpr std::size_t kMaxIdLength = 64;
    auto read_id = [&](std::size_t& pos, std::string& out) -> bool {
        std::string s;
        while (pos < b.size() && b[pos] != 0) {
            const std::uint8_t c = b[pos];
            if (c < 0x20 || c > 0x7E || s.size() >= kMaxIdLength) return false;
            s.push_back(char(c));
            ++pos;
        }
        if (pos >= b.size()) return false;
        ++pos;
        out = s;
        return true;
    };

    std::size_t pos = (b.size() > kMapInfoStringsOffset && b[kMapInfoStringsOffset - 1] != 0)
                          ? kMapInfoStringsOffset - 1
                          : kMapInfoStringsOffset;
    std::string fog, tileset;
    const bool strings_ok = b.size() > kMapInfoStringsOffset && read_id(pos, fog) &&
                            read_id(pos, tileset) && !tileset.empty();
    if (strings_ok && pos + 16 <= b.size()) {
        const std::int32_t l = std::int32_t(u32(pos));
        const std::int32_t bt = std::int32_t(u32(pos + 4));
        const std::int32_t r = std::int32_t(u32(pos + 8));
        const std::int32_t t = std::int32_t(u32(pos + 12));
        const bool inside = l >= 0 && bt >= 0 && r > l && t > bt &&
                            std::uint32_t(r) <= h.width && std::uint32_t(t) <= h.height;
        if (inside) {
            h.fog_type = fog;
            h.tileset = tileset;
            h.camera_left = l;
            h.camera_bottom = bt;
            h.camera_right = r;
            h.camera_top = t;
            h.has_camera_bounds = true;

            auto snap_up = [](std::uint32_t v) { return v + (v % kPlayableSnap); };
            auto snap_down = [](std::uint32_t v) { return v - (v % kPlayableSnap); };
            const std::uint32_t x0 = snap_up(std::uint32_t(l));
            const std::uint32_t y0 = snap_up(std::uint32_t(bt));
            const std::uint32_t x1 = snap_down(std::uint32_t(r));
            const std::uint32_t y1 = snap_down(std::uint32_t(t));
            const std::uint32_t max_x = h.width > kPlayableEdgeMargin ? h.width - kPlayableEdgeMargin : 0;
            const std::uint32_t max_y = h.height > kPlayableEdgeMargin ? h.height - kPlayableEdgeMargin : 0;
            h.playable_x0 = x0 > kPlayableEdgeMargin ? x0 : kPlayableEdgeMargin;
            h.playable_y0 = y0 > kPlayableEdgeMargin ? y0 : kPlayableEdgeMargin;
            h.playable_x1 = x1 < max_x ? x1 : max_x;
            h.playable_y1 = y1 < max_y ? y1 : max_y;
            if (pos + 20 <= b.size()) {
                h.base_height = float(u32(pos + 16)) / 4096.0f;
                h.has_base_height = true;
            }
            if (h.version == 0x27) {
                std::size_t q = pos + 20;
                auto skip_cstr = [&](std::size_t& at) {
                    while (at < b.size() && b[at] != 0) ++at;
                    if (at >= b.size()) return false;
                    ++at;
                    return true;
                };
                bool ok = q + 4 <= b.size();
                if (ok) q += 4;
                ok = ok && skip_cstr(q) && q + 2 + 28 + 32 + 4 <= b.size();
                if (ok) {
                    q += 2 + 28 + 32;
                    const std::uint32_t n = u32(q);
                    q += 4;
                    std::array<std::int32_t, 16> col;
                    col.fill(-1);
                    ok = n >= 1 && n <= 16;
                    for (std::uint32_t i = 0; ok && i < n; ++i) {
                        if (q + 9 > b.size()) { ok = false; break; }
                        const std::uint8_t pid = b[q];
                        const std::uint32_t control = u32(q + 1);
                        const std::int32_t c = std::int32_t(u32(q + 5));
                        q += 9;
                        ok = pid < 16 && control <= 5 && c >= -1 && c < 16 && skip_cstr(q) && q + 12 <= b.size();
                        if (!ok) break;
                        q += 12;
                        ok = skip_cstr(q);
                        if (ok && control == 1) col[pid] = c;
                    }
                    if (ok) {
                        h.player_color = col;
                        h.has_players = true;
                    }
                }
            }
        }
    }
    return h;
}

}
