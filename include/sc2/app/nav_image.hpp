#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "sc2/sim/nav.hpp"

namespace sc2::app {

struct NavImage {
    std::uint32_t width = 0, height = 0;
    std::vector<std::uint8_t> rgba;
};

struct NavColour {
    std::uint8_t r, g, b;
};

enum class NavClass : std::uint8_t {
    CliffWall,
    Blocked,
    Ramp,
    Walkable,
};

inline NavColour nav_class_colour(NavClass c) {
    switch (c) {
        case NavClass::CliffWall: return {96, 16, 16};
        case NavClass::Blocked:   return {200, 48, 48};
        case NavClass::Ramp:      return {240, 216, 64};
        case NavClass::Walkable:  break;
    }
    return {32, 96, 32};
}

inline NavClass nav_cell_class(const sim::NavGrid& nav, int cx, int cy) {
    if (nav.is_blocked_cell(cx, cy))
        return nav.is_cliff_cell(cx, cy) ? NavClass::CliffWall : NavClass::Blocked;
    if (nav.is_ramp_cell(cx, cy)) return NavClass::Ramp;
    return NavClass::Walkable;
}

namespace detail {

template <typename Shade>
inline NavImage rasterise(int w, int h, int scale, Shade shade) {
    NavImage im;
    if (w <= 0 || h <= 0 || scale <= 0) return im;
    im.width = std::uint32_t(w) * std::uint32_t(scale);
    im.height = std::uint32_t(h) * std::uint32_t(scale);
    im.rgba.assign(std::size_t(im.width) * im.height * 4, 255);
    for (int cy = 0; cy < h; ++cy) {
        for (int cx = 0; cx < w; ++cx) {
            const NavColour col = shade(cx, cy);
            const int base_py = (h - 1 - cy) * scale;
            const int base_px = cx * scale;
            for (int dy = 0; dy < scale; ++dy) {
                std::size_t o = (std::size_t(base_py + dy) * im.width + std::size_t(base_px)) * 4;
                for (int dx = 0; dx < scale; ++dx, o += 4) {
                    im.rgba[o] = col.r;
                    im.rgba[o + 1] = col.g;
                    im.rgba[o + 2] = col.b;
                    im.rgba[o + 3] = 255;
                }
            }
        }
    }
    return im;
}

}

inline NavImage nav_pathing_image(const sim::NavGrid& nav, int scale = 1) {
    return detail::rasterise(nav.width(), nav.height(), scale, [&](int cx, int cy) {
        return nav_class_colour(nav_cell_class(nav, cx, cy));
    });
}

inline NavImage nav_tier_image(const sim::NavGrid& nav, int scale = 1) {
    int max_level = 0;
    for (int cy = 0; cy < nav.height(); ++cy)
        for (int cx = 0; cx < nav.width(); ++cx) {
            const int lv = int(nav.level_cell(cx, cy));
            if (lv > max_level) max_level = lv;
        }
    const int step = max_level > 0 ? 255 / max_level : 0;
    return detail::rasterise(nav.width(), nav.height(), scale, [&](int cx, int cy) {
        const int v = int(nav.level_cell(cx, cy)) * step;
        const std::uint8_t g = std::uint8_t(v > 255 ? 255 : v);
        return NavColour{g, g, g};
    });
}

}
