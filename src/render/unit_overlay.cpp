#include "sc2/render/unit_overlay.hpp"

#include <algorithm>
#include <cmath>

namespace sc2::render {

namespace {

struct Color {
    uint8_t r, g, b, a;
};

constexpr Color kGreen{60, 220, 60, 255};
constexpr Color kRed{220, 60, 60, 255};

bool project(const Mat4& mvp, float x, float y, float z, uint32_t view_w,
             uint32_t view_h, float& px, float& py) {
    const auto& m = mvp.m;
    float cx = m[0] * x + m[4] * y + m[8] * z + m[12];
    float cy = m[1] * x + m[5] * y + m[9] * z + m[13];
    float cw = m[3] * x + m[7] * y + m[11] * z + m[15];
    if (cw <= 1e-6f) return false;
    px = (cx / cw * 0.5f + 0.5f) * float(view_w);
    py = (cy / cw * 0.5f + 0.5f) * float(view_h);
    return true;
}

void blend(std::vector<uint8_t>& buf, uint32_t w, uint32_t h, int x, int y,
           const Color& c) {
    if (x < 0 || y < 0 || x >= int(w) || y >= int(h)) return;
    const size_t i = (size_t(y) * size_t(w) + size_t(x)) * 4;
    const int a = c.a;
    const int ia = 255 - a;
    buf[i + 0] = uint8_t((int(c.r) * a + int(buf[i + 0]) * ia) / 255);
    buf[i + 1] = uint8_t((int(c.g) * a + int(buf[i + 1]) * ia) / 255);
    buf[i + 2] = uint8_t((int(c.b) * a + int(buf[i + 2]) * ia) / 255);
    buf[i + 3] = uint8_t(a + int(buf[i + 3]) * ia / 255);
}

void fill_rect(std::vector<uint8_t>& buf, uint32_t w, uint32_t h, int x0, int y0,
               int x1, int y1, const Color& c) {
    for (int y = y0; y < y1; ++y)
        for (int x = x0; x < x1; ++x) blend(buf, w, h, x, y, c);
}

void draw_ring(std::vector<uint8_t>& buf, uint32_t w, uint32_t h, int cx, int cy,
               int radius, const Color& c) {
    const int span = radius + 2;
    for (int dy = -span; dy <= span; ++dy) {
        for (int dx = -span; dx <= span; ++dx) {
            float d = std::sqrt(float(dx * dx + dy * dy));
            if (std::fabs(d - float(radius)) <= UnitOverlay::kRingThickness)
                blend(buf, w, h, cx + dx, cy + dy, c);
        }
    }
}

void draw_hp_bar(std::vector<uint8_t>& buf, uint32_t w, uint32_t h, int cx, int cy,
                 double frac) {
    if (frac < 0.0) frac = 0.0;
    if (frac > 1.0) frac = 1.0;
    const int total = UnitOverlay::kBarWidth;
    int green_w = int(std::lround(frac * double(total)));
    if (green_w < 0) green_w = 0;
    if (green_w > total) green_w = total;

    const int left = cx - total / 2;
    const int top = cy - UnitOverlay::kBarOffsetY - UnitOverlay::kBarHeight;
    const int bottom = top + UnitOverlay::kBarHeight;
    fill_rect(buf, w, h, left, top, left + green_w, bottom, kGreen);
    fill_rect(buf, w, h, left + green_w, top, left + total, bottom, kRed);
}

}

std::vector<uint8_t> UnitOverlay::render(const sim::World& w, const Mat4& mvp,
                                         uint32_t view_w, uint32_t view_h,
                                         const std::vector<uint32_t>& selection,
                                         int viewer_player) const {
    std::vector<uint8_t> rgba(size_t(view_w) * size_t(view_h) * 4, 0);
    if (view_w == 0 || view_h == 0) return rgba;

    for (const sim::Unit& u : w.units()) {
        if (!u.alive) continue;

        float px, py;
        if (!project(mvp, float(u.x.to_double()), float(u.y.to_double()), 0.0f, view_w,
                     view_h, px, py))
            continue;
        const int cx = int(std::lround(px));
        const int cy = int(std::lround(py));
        if (cx < 0 || cy < 0 || cx >= int(view_w) || cy >= int(view_h))
            continue;

        if (std::find(selection.begin(), selection.end(), u.id) != selection.end()) {
            const Color ring = (u.player == viewer_player) ? kGreen : kRed;
            int radius = kRingBaseRadius +
                         int(std::lround(u.radius.to_double() * kRingRadiusPerWorld));
            if (radius > kRingMaxRadius) radius = kRingMaxRadius;
            if (radius < 1) radius = 1;
            draw_ring(rgba, view_w, view_h, cx, cy, radius, ring);
        }

        if (u.player >= 0 && u.hp_max.raw() > 0 && u.hp < u.hp_max) {
            const double frac = u.hp.to_double() / u.hp_max.to_double();
            draw_hp_bar(rgba, view_w, view_h, cx, cy, frac);
        }
    }
    return rgba;
}

}
