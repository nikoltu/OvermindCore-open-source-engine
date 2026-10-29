#pragma once

#include <cstdint>
#include <vector>

#include "omc/sim/fixed.hpp"
#include "omc/sim/nav.hpp"

namespace omc::sim {

enum TerrainFlag : std::uint8_t {
    kPathable = 1u << 0,
    kBuildable = 1u << 1,
    kWater = 1u << 2,
    kRamp = 1u << 3,
    kBlocker = 1u << 4,
};

class TerrainMap {
public:
    void configure(int w, int h, Fixed cell_size, Fixed origin_x, Fixed origin_y) {
        w_ = w;
        h_ = h;
        cell_ = cell_size;
        ox_ = origin_x;
        oy_ = origin_y;
        level_.assign(size_t(w) * size_t(h), 0);
        flags_.assign(size_t(w) * size_t(h), std::uint8_t(kPathable | kBuildable));
    }

    int width() const { return w_; }
    int height() const { return h_; }
    bool configured() const { return w_ > 0 && h_ > 0; }

    bool in_bounds(int cx, int cy) const { return cx >= 0 && cy >= 0 && cx < w_ && cy < h_; }

    void to_cell(Fixed wx, Fixed wy, int& cx, int& cy) const {
        cx = ((wx - ox_) / cell_).floor();
        cy = ((wy - oy_) / cell_).floor();
    }
    void cell_center(int cx, int cy, Fixed& wx, Fixed& wy) const {
        Fixed half = cell_ / Fixed::from_int(2);
        wx = ox_ + Fixed::from_int(cx) * cell_ + half;
        wy = oy_ + Fixed::from_int(cy) * cell_ + half;
    }

    std::uint8_t level(int cx, int cy) const {
        return in_bounds(cx, cy) ? level_[idx(cx, cy)] : 0;
    }
    std::uint8_t level_at(Fixed wx, Fixed wy) const {
        int cx, cy;
        to_cell(wx, wy, cx, cy);
        return level(cx, cy);
    }
    void set_level(int cx, int cy, std::uint8_t l) {
        if (in_bounds(cx, cy)) level_[idx(cx, cy)] = l;
    }

    std::uint8_t flags(int cx, int cy) const {
        return in_bounds(cx, cy) ? flags_[idx(cx, cy)] : 0;
    }
    bool test(int cx, int cy, TerrainFlag f) const { return (flags(cx, cy) & f) != 0; }
    void set_flag(int cx, int cy, TerrainFlag f) {
        if (in_bounds(cx, cy)) flags_[idx(cx, cy)] |= f;
    }
    void clear_flag(int cx, int cy, TerrainFlag f) {
        if (in_bounds(cx, cy)) flags_[idx(cx, cy)] &= std::uint8_t(~f);
    }

    bool is_pathable(int cx, int cy) const {
        return test(cx, cy, kPathable) && !test(cx, cy, kBlocker) && !test(cx, cy, kWater);
    }
    bool is_buildable(int cx, int cy) const {
        return test(cx, cy, kBuildable) && !test(cx, cy, kBlocker) && !test(cx, cy, kWater);
    }
    bool is_water(int cx, int cy) const { return test(cx, cy, kWater); }
    bool is_ramp(int cx, int cy) const { return test(cx, cy, kRamp); }

    bool can_place(int cx0, int cy0, int fw, int fh, bool ignore_buildable = false) const {
        if (fw <= 0 || fh <= 0) return false;
        if (!in_bounds(cx0, cy0) || !in_bounds(cx0 + fw - 1, cy0 + fh - 1)) return false;
        const std::uint8_t base = level(cx0, cy0);
        for (int y = cy0; y < cy0 + fh; ++y)
            for (int x = cx0; x < cx0 + fw; ++x) {
                if (!ignore_buildable && !is_buildable(x, y)) return false;
                if (level(x, y) != base) return false;
            }
        return true;
    }
    bool can_place_world(Fixed wx, Fixed wy, int fw, int fh) const {
        int cx, cy;
        to_cell(wx, wy, cx, cy);
        return can_place(cx, cy, fw, fh);
    }

    void apply_pathing(NavGrid& g) const {
        for (int y = 0; y < h_; ++y)
            for (int x = 0; x < w_; ++x)
                if (!is_pathable(x, y)) g.set_blocked(x, y, true);
    }

    bool vision_reaches(std::uint8_t viewer_level, int tx, int ty) const {
        if (!in_bounds(tx, ty)) return false;
        if (is_ramp(tx, ty)) return true;
        return level(tx, ty) <= viewer_level;
    }

    std::uint64_t checksum() const {
        std::uint64_t h = 1469598103934665603ull;
        auto fold = [&](std::uint8_t b) { h ^= b; h *= 1099511628211ull; };
        for (std::uint8_t l : level_) fold(l);
        for (std::uint8_t f : flags_) fold(f);
        return h;
    }

private:
    size_t idx(int cx, int cy) const { return size_t(cy) * size_t(w_) + size_t(cx); }

    int w_ = 0, h_ = 0;
    Fixed cell_ = Fixed::from_int(1);
    Fixed ox_, oy_;
    std::vector<std::uint8_t> level_;
    std::vector<std::uint8_t> flags_;
};

}
