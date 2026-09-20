#pragma once

#include <climits>
#include <cmath>
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

inline int& forced_font_px() {
    static int px = 0;
    return px;
}
inline void set_forced_font_px(int px) { forced_font_px() = px > 0 ? px : 0; }

inline int font_px_for(int sc) {
    const int f = forced_font_px();
    return f > 0 ? f : real_font_px(sc);
}

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

inline void fill_a(std::vector<std::uint8_t>& b, std::uint32_t w, std::uint32_t h, int x0, int y0,
                   int x1, int y1, std::uint8_t r, std::uint8_t g, std::uint8_t bl,
                   std::uint8_t a) {
    for (int y = y0; y < y1; ++y)
        for (int x = x0; x < x1; ++x) blend(b, w, h, x, y, r, g, bl, a);
}

inline void vgrad(std::vector<std::uint8_t>& b, std::uint32_t w, std::uint32_t h, int x0, int y0,
                  int x1, int y1, std::uint8_t r0, std::uint8_t g0, std::uint8_t b0,
                  std::uint8_t r1, std::uint8_t g1, std::uint8_t b1, std::uint8_t a0 = 255,
                  int a1 = -1) {
    const int span = y1 - 1 - y0;
    const int ae = a1 < 0 ? int(a0) : a1;
    for (int y = y0; y < y1; ++y) {
        const int num = span > 0 ? (y - y0) : 0;
        const int den = span > 0 ? span : 1;
        std::uint8_t r = std::uint8_t(int(r0) + (int(r1) - int(r0)) * num / den);
        std::uint8_t g = std::uint8_t(int(g0) + (int(g1) - int(g0)) * num / den);
        std::uint8_t bb = std::uint8_t(int(b0) + (int(b1) - int(b0)) * num / den);
        std::uint8_t a = std::uint8_t(int(a0) + (ae - int(a0)) * num / den);
        for (int x = x0; x < x1; ++x) blend(b, w, h, x, y, r, g, bb, a);
    }
}

inline void box_a(std::vector<std::uint8_t>& b, std::uint32_t w, std::uint32_t h, int x0, int y0,
                  int x1, int y1, std::uint8_t r, std::uint8_t g, std::uint8_t bl,
                  std::uint8_t a = 255) {
    for (int x = x0; x < x1; ++x) {
        blend(b, w, h, x, y0, r, g, bl, a);
        blend(b, w, h, x, y1 - 1, r, g, bl, a);
    }
    for (int y = y0; y < y1; ++y) {
        blend(b, w, h, x0, y, r, g, bl, a);
        blend(b, w, h, x1 - 1, y, r, g, bl, a);
    }
}

inline void inset_hi(std::vector<std::uint8_t>& b, std::uint32_t w, std::uint32_t h, int x0,
                     int y0, int x1, int y1, std::uint8_t r, std::uint8_t g, std::uint8_t bl,
                     std::uint8_t a) {
    if (y1 - y0 < 3) return;
    for (int x = x0 + 1; x < x1 - 1; ++x) blend(b, w, h, x, y0 + 1, r, g, bl, a);
}

inline int text_tracked(std::vector<std::uint8_t>& b, std::uint32_t w, std::uint32_t h, int x,
                        int y, const std::string& s, int sc, int track_px, std::uint8_t r,
                        std::uint8_t g, std::uint8_t bl, int cx0 = INT_MIN, int cy0 = INT_MIN,
                        int cx1 = INT_MAX, int cy1 = INT_MAX) {
    auto put = [&](int dx, int dy, std::uint8_t cr, std::uint8_t cg, std::uint8_t cb,
                   std::uint8_t ca) {
        if (dx < cx0 || dx >= cx1 || dy < cy0 || dy >= cy1) return;
        blend(b, w, h, dx, dy, cr, cg, cb, ca);
    };
    const render::OverlayFont* font = active_font();
    if (font && font->valid()) {
        int H = font_px_for(sc);
        int baseline = y + font->ascent_px(H);
        int pen = x;
        for (std::size_t k = 0; k < s.size(); ++k) {
            if (pen >= cx1) break;
            const render::GlyphBitmap& gl = font->glyph(s[k], H);
            for (int gy = 0; gy < gl.height; ++gy)
                for (int gx = 0; gx < gl.width; ++gx) {
                    std::uint8_t cov = gl.coverage[std::size_t(gy) * gl.width + gx];
                    if (!cov) continue;
                    int dx = pen + gl.bearing_x + gx;
                    int dy = baseline - gl.bearing_y + gy;
                    put(dx + 1, dy + 1, 0, 0, 0, cov);
                    put(dx, dy, r, g, bl, cov);
                }
            pen += gl.advance;
            if (k + 1 < s.size()) pen += track_px;
        }
        return pen;
    }
    int pen = x;
    for (std::size_t k = 0; k < s.size(); ++k) {
        if (pen >= cx1) break;
        for (int row = 0; row < ui::kFontH; ++row)
            for (int col = 0; col < ui::kFontW; ++col)
                if (ui::font_pixel(s[k], col, row))
                    for (int dy = 0; dy < sc; ++dy)
                        for (int dx = 0; dx < sc; ++dx) {
                            put(pen + col * sc + dx + sc, y + row * sc + dy + sc, 0, 0, 0, 255);
                            put(pen + col * sc + dx, y + row * sc + dy, r, g, bl, 255);
                        }
        pen += (ui::kFontW + 1) * sc;
        if (k + 1 < s.size()) pen += track_px;
    }
    return pen;
}

inline int text(std::vector<std::uint8_t>& b, std::uint32_t w, std::uint32_t h, int x, int y,
                const std::string& s, int sc, std::uint8_t r, std::uint8_t g, std::uint8_t bl,
                int cx0 = INT_MIN, int cy0 = INT_MIN, int cx1 = INT_MAX, int cy1 = INT_MAX) {
    return text_tracked(b, w, h, x, y, s, sc, 0, r, g, bl, cx0, cy0, cx1, cy1);
}

inline int text_w_tracked(const std::string& s, int sc, int track_px) {
    int wsum = 0;
    const render::OverlayFont* font = active_font();
    if (font && font->valid()) {
        int H = font_px_for(sc);
        for (char ch : s) wsum += font->advance_px(ch, H);
    } else {
        wsum = int(s.size()) * (ui::kFontW + 1) * sc;
    }
    if (s.size() > 1) wsum += track_px * int(s.size() - 1);
    return wsum;
}

inline int text_w(const std::string& s, int sc) { return text_w_tracked(s, sc, 0); }

inline int bitmap_scale_for_px(int em_px) {
    const int sc = em_px / 10;
    return sc > 0 ? sc : 1;
}
inline int track_px_for(int em_px, float track_em) {
    return int(std::lround(double(track_em) * double(em_px)));
}

inline int text_em(std::vector<std::uint8_t>& b, std::uint32_t w, std::uint32_t h, int x, int y,
                   const std::string& s, int em_px, float track_em, std::uint8_t r,
                   std::uint8_t g, std::uint8_t bl, int cx0 = INT_MIN, int cy0 = INT_MIN,
                   int cx1 = INT_MAX, int cy1 = INT_MAX) {
    const int saved = forced_font_px();
    set_forced_font_px(em_px);
    const int pen = text_tracked(b, w, h, x, y, s, bitmap_scale_for_px(em_px),
                                 track_px_for(em_px, track_em), r, g, bl, cx0, cy0, cx1, cy1);
    set_forced_font_px(saved);
    return pen;
}

inline int text_w_em(const std::string& s, int em_px, float track_em) {
    const int saved = forced_font_px();
    set_forced_font_px(em_px);
    const int wpx = text_w_tracked(s, bitmap_scale_for_px(em_px), track_px_for(em_px, track_em));
    set_forced_font_px(saved);
    return wpx;
}

inline int text_h_em(int em_px) {
    const render::OverlayFont* font = active_font();
    if (font && font->valid()) return em_px;
    return ui::kFontH * bitmap_scale_for_px(em_px);
}

}
