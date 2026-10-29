#include "omc/render/overlay.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace omc::render {

namespace {

struct Rgb {
    uint8_t r, g, b;
};

uint8_t to_u8(float f) {
    if (f <= 0.0f) return 0;
    if (f >= 1.0f) return 255;
    return static_cast<uint8_t>(f * 255.0f + 0.5f);
}

float clamp01(float f) { return f < 0.0f ? 0.0f : (f > 1.0f ? 1.0f : f); }

void put_pixel(RenderResult& img, int x, int y, Rgb c) {
    if (x < 0 || y < 0 || x >= static_cast<int>(img.width) ||
        y >= static_cast<int>(img.height))
        return;
    std::size_t i =
        (static_cast<std::size_t>(y) * img.width + static_cast<std::size_t>(x)) * 4;
    img.rgba[i + 0] = c.r;
    img.rgba[i + 1] = c.g;
    img.rgba[i + 2] = c.b;
    img.rgba[i + 3] = 255;
}

void put_brush(RenderResult& img, int x, int y, int t, Rgb c) {
    for (int dy = -t; dy <= t; ++dy)
        for (int dx = -t; dx <= t; ++dx) put_pixel(img, x + dx, y + dy, c);
}

void draw_line(RenderResult& img, float x0, float y0, float x1, float y1, int t, Rgb c) {
    float dx = x1 - x0, dy = y1 - y0;
    int steps = static_cast<int>(std::max(std::fabs(dx), std::fabs(dy))) + 1;
    for (int i = 0; i <= steps; ++i) {
        float f = static_cast<float>(i) / static_cast<float>(steps);
        put_brush(img, static_cast<int>(std::lround(x0 + dx * f)),
                  static_cast<int>(std::lround(y0 + dy * f)), t, c);
    }
}

void fill_rect(RenderResult& img, int x0, int y0, int x1, int y1, Rgb c) {
    for (int y = y0; y < y1; ++y)
        for (int x = x0; x < x1; ++x) put_pixel(img, x, y, c);
}

struct ScreenPt {
    float x, y;
    bool ok;
};

}

void draw_overlay(RenderResult& result, const std::vector<OverlayUnit>& units) {
    if (result.width == 0 || result.height == 0) return;
    if (result.rgba.size() <
        static_cast<std::size_t>(result.width) * result.height * 4)
        return;

    const int w = static_cast<int>(result.width);
    const int h = static_cast<int>(result.height);
    constexpr int kSamples = 128;
    constexpr int kBarW = 24;
    constexpr int kBarH = 4;
    constexpr int kBarGap = 4;
    constexpr float kPi = 3.14159265358979323846f;

    for (const auto& u : units) {
        float cx, cy;
        if (!project(result.mvp, u.world_pos[0], u.world_pos[1], u.world_pos[2],
                     result.width, result.height, cx, cy))
            continue;
        if (cx < 0.0f || cy < 0.0f || cx >= static_cast<float>(w) ||
            cy >= static_cast<float>(h))
            continue;

        ScreenPt ring[kSamples];
        float top_y = cy;
        for (int i = 0; i < kSamples; ++i) {
            float a = 2.0f * kPi * static_cast<float>(i) / static_cast<float>(kSamples);
            float px, py;
            bool ok = project(result.mvp, u.world_pos[0] + u.ground_radius * std::cos(a),
                              u.world_pos[1] + u.ground_radius * std::sin(a),
                              u.world_pos[2], result.width, result.height, px, py);
            ring[i] = {px, py, ok};
            if (ok && py < top_y) top_y = py;
        }

        Rgb team{to_u8(u.team_rgb[0]), to_u8(u.team_rgb[1]), to_u8(u.team_rgb[2])};

        if (u.selected) {
            for (int i = 0; i < kSamples; ++i) {
                const ScreenPt& a = ring[i];
                const ScreenPt& b = ring[(i + 1) % kSamples];
                if (a.ok && b.ok) draw_line(result, a.x, a.y, b.x, b.y, 0, team);
            }
        }

        float hp = clamp01(u.hp_fraction);
        int bar_bottom = static_cast<int>(std::lround(top_y)) - kBarGap;
        int bar_top = bar_bottom - kBarH;
        int bar_left = static_cast<int>(std::lround(cx)) - kBarW / 2;
        int bar_right = bar_left + kBarW;
        fill_rect(result, bar_left - 1, bar_top - 1, bar_right + 1, bar_bottom + 1,
                  Rgb{0, 0, 0});
        fill_rect(result, bar_left, bar_top, bar_right, bar_bottom, Rgb{40, 40, 40});
        int fill_w = static_cast<int>(std::lround(hp * static_cast<float>(kBarW)));
        Rgb hp_col{to_u8(1.0f - hp), to_u8(hp), 0};
        fill_rect(result, bar_left, bar_top, bar_left + fill_w, bar_bottom, hp_col);
    }
}

}
