#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "sc2/render/font_ttf.hpp"
#include "sc2/ui/font.hpp"

namespace sc2::app::draw {

inline const render::OverlayFont*& active_font() {
    static const render::OverlayFont* f = nullptr;
    return f;
}
inline void set_active_font(const render::OverlayFont* f) { active_font() = f; }

inline int real_font_px(int sc) { return sc * 10; }

inline void px(std::vector<std::uint8_t>& b, std::uint32_t w, std::uint32_t h, int x, int y,
               std::uint8_t r, std::uint8_t g, std::uint8_t bl, std::uint8_t a) {
    if (x < 0 || y < 0 || x >= int(w) || y >= int(h)) return;
    std::size_t i = (std::size_t(y) * w + x) * 4;
    b[i] = r; b[i + 1] = g; b[i + 2] = bl; b[i + 3] = a;
}

inline void fill(std::vector<std::uint8_t>& b, std::uint32_t w, std::uint32_t h, int x0, int y0,
                 int x1, int y1, std::uint8_t r, std::uint8_t g, std::uint8_t bl, std::uint8_t a) {
    for (int y = y0; y < y1; ++y)
        for (int x = x0; x < x1; ++x) px(b, w, h, x, y, r, g, bl, a);
}

inline void box(std::vector<std::uint8_t>& b, std::uint32_t w, std::uint32_t h, int x0, int y0,
                int x1, int y1, std::uint8_t r, std::uint8_t g, std::uint8_t bl) {
    for (int x = x0; x < x1; ++x) { px(b, w, h, x, y0, r, g, bl, 255); px(b, w, h, x, y1 - 1, r, g, bl, 255); }
    for (int y = y0; y < y1; ++y) { px(b, w, h, x0, y, r, g, bl, 255); px(b, w, h, x1 - 1, y, r, g, bl, 255); }
}

inline void blend(std::vector<std::uint8_t>& b, std::uint32_t w, std::uint32_t h, int x, int y,
                  std::uint8_t r, std::uint8_t g, std::uint8_t bl, std::uint8_t a) {
    if (a == 0 || x < 0 || y < 0 || x >= int(w) || y >= int(h)) return;
    std::size_t i = (std::size_t(y) * w + x) * 4;
    std::uint32_t ia = 255u - a;
    b[i] = std::uint8_t((r * a + b[i] * ia) / 255);
    b[i + 1] = std::uint8_t((g * a + b[i + 1] * ia) / 255);
    b[i + 2] = std::uint8_t((bl * a + b[i + 2] * ia) / 255);
    std::uint32_t da = b[i + 3];
    b[i + 3] = std::uint8_t(a + da * ia / 255);
}

inline void text(std::vector<std::uint8_t>& b, std::uint32_t w, std::uint32_t h, int x, int y,
                 const std::string& s, int sc, std::uint8_t r, std::uint8_t g, std::uint8_t bl) {
    const render::OverlayFont* font = active_font();
    if (font && font->valid()) {
        int H = real_font_px(sc);
        int baseline = y + font->ascent_px(H);
        int pen = x;
        for (char ch : s) {
            const render::GlyphBitmap& gl = font->glyph(ch, H);
            for (int gy = 0; gy < gl.height; ++gy)
                for (int gx = 0; gx < gl.width; ++gx) {
                    std::uint8_t cov = gl.coverage[std::size_t(gy) * gl.width + gx];
                    if (!cov) continue;
                    int dx = pen + gl.bearing_x + gx;
                    int dy = baseline - gl.bearing_y + gy;
                    blend(b, w, h, dx + 1, dy + 1, 0, 0, 0, cov);
                    blend(b, w, h, dx, dy, r, g, bl, cov);
                }
            pen += gl.advance;
        }
        return;
    }
    int pen = x;
    for (char ch : s) {
        for (int row = 0; row < ui::kFontH; ++row)
            for (int col = 0; col < ui::kFontW; ++col)
                if (ui::font_pixel(ch, col, row))
                    for (int dy = 0; dy < sc; ++dy)
                        for (int dx = 0; dx < sc; ++dx) {
                            px(b, w, h, pen + col * sc + dx + sc, y + row * sc + dy + sc, 0, 0, 0, 255);
                            px(b, w, h, pen + col * sc + dx, y + row * sc + dy, r, g, bl, 255);
                        }
        pen += (ui::kFontW + 1) * sc;
    }
}

inline int text_w(const std::string& s, int sc) {
    const render::OverlayFont* font = active_font();
    if (font && font->valid()) {
        int H = real_font_px(sc);
        int wsum = 0;
        for (char ch : s) wsum += font->advance_px(ch, H);
        return wsum;
    }
    return int(s.size()) * (ui::kFontW + 1) * sc;
}

}
