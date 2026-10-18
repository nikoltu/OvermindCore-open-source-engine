#pragma once

#include <climits>
#include <cmath>
#include <cstdint>
#include <cstdlib>
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

inline bool& text_shadow_off() {
    static bool off = false;
    return off;
}

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
        static const struct CovLut {
            std::uint8_t v[256];
            CovLut() {
                const bool linear = std::getenv("SC2_TEXT_COVERAGE_LINEAR") != nullptr;
                for (int i = 0; i < 256; ++i)
                    v[i] = linear ? std::uint8_t(i) : std::uint8_t(std::pow(double(i) / 255.0, 0.6) * 255.0 + 0.5);
            }
        } cov_lut;
        for (std::size_t k = 0; k < s.size(); ++k) {
            if (pen >= cx1) break;
            const render::GlyphBitmap& gl = font->glyph(s[k], H);
            for (int gy = 0; gy < gl.height; ++gy)
                for (int gx = 0; gx < gl.width; ++gx) {
                    std::uint8_t cov = cov_lut.v[gl.coverage[std::size_t(gy) * gl.width + gx]];
                    if (!cov) continue;
                    int dx = pen + gl.bearing_x + gx;
                    int dy = baseline - gl.bearing_y + gy;
                    if (!text_shadow_off()) put(dx + 1, dy + 1, 0, 0, 0, cov);
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

struct ClipRect {
    int x0 = INT_MIN, y0 = INT_MIN, x1 = INT_MAX, y1 = INT_MAX;
};
inline std::vector<ClipRect>& clip_stack() {
    static std::vector<ClipRect> s;
    return s;
}
inline ClipRect clip_rect() { return clip_stack().empty() ? ClipRect{} : clip_stack().back(); }
inline void clip_push(int x0, int y0, int x1, int y1) {
    const ClipRect c = clip_rect();
    clip_stack().push_back(ClipRect{x0 > c.x0 ? x0 : c.x0, y0 > c.y0 ? y0 : c.y0, x1 < c.x1 ? x1 : c.x1, y1 < c.y1 ? y1 : c.y1});
}
inline void clip_pop() {
    if (!clip_stack().empty()) clip_stack().pop_back();
}

inline bool clip_span(std::uint32_t w, std::uint32_t h, int& x0, int& y0, int& x1, int& y1) {
    const ClipRect c = clip_rect();
    if (x0 < c.x0) x0 = c.x0;
    if (y0 < c.y0) y0 = c.y0;
    if (x1 > c.x1) x1 = c.x1;
    if (y1 > c.y1) y1 = c.y1;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > int(w)) x1 = int(w);
    if (y1 > int(h)) y1 = int(h);
    return x1 > x0 && y1 > y0;
}

inline void blend_raw(std::uint8_t* p, std::uint8_t r, std::uint8_t g, std::uint8_t bl, std::uint8_t a) {
    if (a == 255) {
        p[0] = r; p[1] = g; p[2] = bl; p[3] = 255;
        return;
    }
    if (a == 0) return;
    const std::uint32_t ia = 255u - a;
    p[0] = std::uint8_t((r * a + p[0] * ia) / 255);
    p[1] = std::uint8_t((g * a + p[1] * ia) / 255);
    p[2] = std::uint8_t((bl * a + p[2] * ia) / 255);
    p[3] = std::uint8_t(a + p[3] * ia / 255);
}

inline void fill_a_c(std::vector<std::uint8_t>& b, std::uint32_t w, std::uint32_t h, int x0, int y0, int x1, int y1,
                     std::uint8_t r, std::uint8_t g, std::uint8_t bl, std::uint8_t a = 255) {
    if (!clip_span(w, h, x0, y0, x1, y1)) return;
    for (int y = y0; y < y1; ++y) {
        std::uint8_t* p = &b[(std::size_t(y) * w + std::size_t(x0)) * 4];
        for (int x = x0; x < x1; ++x, p += 4) blend_raw(p, r, g, bl, a);
    }
}
inline void fill_c(std::vector<std::uint8_t>& b, std::uint32_t w, std::uint32_t h, int x0, int y0, int x1, int y1,
                   std::uint8_t r, std::uint8_t g, std::uint8_t bl) {
    fill_a_c(b, w, h, x0, y0, x1, y1, r, g, bl, 255);
}

inline void vgrad_c(std::vector<std::uint8_t>& b, std::uint32_t w, std::uint32_t h, int x0, int y0, int x1, int y1,
                    std::uint8_t r0, std::uint8_t g0, std::uint8_t b0, std::uint8_t r1, std::uint8_t g1, std::uint8_t b1,
                    std::uint8_t a0 = 255, int a1 = -1) {
    const int top = y0, span = y1 - 1 - y0;
    const int ae = a1 < 0 ? int(a0) : a1;
    if (!clip_span(w, h, x0, y0, x1, y1)) return;
    for (int y = y0; y < y1; ++y) {
        const int num = span > 0 ? (y - top) : 0;
        const int den = span > 0 ? span : 1;
        const std::uint8_t r = std::uint8_t(int(r0) + (int(r1) - int(r0)) * num / den);
        const std::uint8_t g = std::uint8_t(int(g0) + (int(g1) - int(g0)) * num / den);
        const std::uint8_t bb = std::uint8_t(int(b0) + (int(b1) - int(b0)) * num / den);
        const std::uint8_t a = std::uint8_t(int(a0) + (ae - int(a0)) * num / den);
        std::uint8_t* p = &b[(std::size_t(y) * w + std::size_t(x0)) * 4];
        for (int x = x0; x < x1; ++x, p += 4) blend_raw(p, r, g, bb, a);
    }
}

inline void hgrad(std::vector<std::uint8_t>& b, std::uint32_t w, std::uint32_t h, int x0, int y0, int x1, int y1,
                  std::uint8_t r0, std::uint8_t g0, std::uint8_t b0, int a0, std::uint8_t r1, std::uint8_t g1, std::uint8_t b1,
                  int a1) {
    const int left = x0, span = x1 - 1 - x0;
    if (!clip_span(w, h, x0, y0, x1, y1)) return;
    for (int x = x0; x < x1; ++x) {
        const int num = span > 0 ? (x - left) : 0;
        const int den = span > 0 ? span : 1;
        const std::uint8_t r = std::uint8_t(int(r0) + (int(r1) - int(r0)) * num / den);
        const std::uint8_t g = std::uint8_t(int(g0) + (int(g1) - int(g0)) * num / den);
        const std::uint8_t bb = std::uint8_t(int(b0) + (int(b1) - int(b0)) * num / den);
        const std::uint8_t a = std::uint8_t(a0 + (a1 - a0) * num / den);
        for (int y = y0; y < y1; ++y) blend_raw(&b[(std::size_t(y) * w + std::size_t(x)) * 4], r, g, bb, a);
    }
}

inline void box_a_c(std::vector<std::uint8_t>& b, std::uint32_t w, std::uint32_t h, int x0, int y0, int x1, int y1,
                    std::uint8_t r, std::uint8_t g, std::uint8_t bl, std::uint8_t a = 255, int t = 1) {
    if (t < 1 || x1 <= x0 || y1 <= y0) return;
    if (t * 2 >= x1 - x0 || t * 2 >= y1 - y0) {
        fill_a_c(b, w, h, x0, y0, x1, y1, r, g, bl, a);
        return;
    }
    fill_a_c(b, w, h, x0, y0, x1, y0 + t, r, g, bl, a);
    fill_a_c(b, w, h, x0, y1 - t, x1, y1, r, g, bl, a);
    fill_a_c(b, w, h, x0, y0 + t, x0 + t, y1 - t, r, g, bl, a);
    fill_a_c(b, w, h, x1 - t, y0 + t, x1, y1 - t, r, g, bl, a);
}

enum class Fit { Contain, Cover };
struct BlitRect {
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
};
inline BlitRect blit_fit(std::vector<std::uint8_t>& b, std::uint32_t w, std::uint32_t h, int x0, int y0, int x1, int y1,
                         const std::uint8_t* src, std::uint32_t sw, std::uint32_t sh, Fit fit = Fit::Contain,
                         float max_scale = 0.0f, std::uint8_t alpha = 255) {
    BlitRect out;
    const int rw = x1 - x0, rh = y1 - y0;
    if (!src || !sw || !sh || rw <= 0 || rh <= 0) return out;
    const double kx = double(rw) / double(sw), ky = double(rh) / double(sh);
    double k = fit == Fit::Contain ? (kx < ky ? kx : ky) : (kx > ky ? kx : ky);
    if (max_scale > 0.0f && k > double(max_scale)) k = double(max_scale);
    int dw = int(std::lround(double(sw) * k)), dh = int(std::lround(double(sh) * k));
    if (dw < 1) dw = 1;
    if (dh < 1) dh = 1;
    const int bx0 = x0 + (rw - dw) / 2, by0 = y0 + (rh - dh) / 2;
    out.x0 = bx0 > x0 ? bx0 : x0;
    out.y0 = by0 > y0 ? by0 : y0;
    out.x1 = bx0 + dw < x1 ? bx0 + dw : x1;
    out.y1 = by0 + dh < y1 ? by0 + dh : y1;
    int cx0 = out.x0, cy0 = out.y0, cx1 = out.x1, cy1 = out.y1;
    if (!clip_span(w, h, cx0, cy0, cx1, cy1)) return out;
    const bool down = dw < int(sw) || dh < int(sh);
    for (int y = cy0; y < cy1; ++y) {
        const int ty = y - by0;
        int sy0 = int(std::int64_t(ty) * sh / dh), sy1 = int((std::int64_t(ty + 1) * sh + dh - 1) / dh);
        if (sy1 <= sy0) sy1 = sy0 + 1;
        if (sy1 > int(sh)) sy1 = int(sh);
        std::uint8_t* p = &b[(std::size_t(y) * w + std::size_t(cx0)) * 4];
        for (int x = cx0; x < cx1; ++x, p += 4) {
            const int tx = x - bx0;
            int sx0 = int(std::int64_t(tx) * sw / dw);
            std::uint32_t r, g, bl, a;
            if (!down) {
                const std::uint8_t* q = &src[(std::size_t(sy0) * sw + std::size_t(sx0)) * 4];
                r = q[0]; g = q[1]; bl = q[2]; a = q[3];
            } else {
                int sx1 = int((std::int64_t(tx + 1) * sw + dw - 1) / dw);
                if (sx1 <= sx0) sx1 = sx0 + 1;
                if (sx1 > int(sw)) sx1 = int(sw);
                std::uint64_t ar = 0, ag = 0, ab = 0, aa = 0;
                for (int sy = sy0; sy < sy1; ++sy) {
                    const std::uint8_t* q = &src[(std::size_t(sy) * sw + std::size_t(sx0)) * 4];
                    for (int sx = sx0; sx < sx1; ++sx, q += 4) {
                        ar += std::uint32_t(q[0]) * q[3];
                        ag += std::uint32_t(q[1]) * q[3];
                        ab += std::uint32_t(q[2]) * q[3];
                        aa += q[3];
                    }
                }
                const std::uint64_t n = std::uint64_t(sx1 - sx0) * std::uint64_t(sy1 - sy0);
                if (!aa) continue;
                r = std::uint32_t((ar + aa / 2) / aa);
                g = std::uint32_t((ag + aa / 2) / aa);
                bl = std::uint32_t((ab + aa / 2) / aa);
                a = std::uint32_t((aa + n / 2) / n);
            }
            if (alpha != 255) a = a * alpha / 255;
            blend_raw(p, std::uint8_t(r), std::uint8_t(g), std::uint8_t(bl), std::uint8_t(a));
        }
    }
    return out;
}

inline std::string text_ellipsis(const std::string& s, int em_px, float track_em, int max_w) {
    if (text_w_em(s, em_px, track_em) <= max_w) return s;
    const std::string dots = "...";
    std::string cut = s;
    while (!cut.empty()) {
        cut.pop_back();
        while (!cut.empty() && cut.back() == ' ') cut.pop_back();
        if (text_w_em(cut + dots, em_px, track_em) <= max_w) return cut + dots;
    }
    return text_w_em(dots, em_px, track_em) <= max_w ? dots : std::string();
}

}
