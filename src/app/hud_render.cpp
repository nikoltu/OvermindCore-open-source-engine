#include "sc2/app/hud_render.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdlib>
#include <string>

#include "sc2/app/console_panel.hpp"
#include "sc2/sim/world.hpp"
#include "sc2/ui/font.hpp"

namespace sc2::app {

namespace {

constexpr int kPad = 6;

int panel_height(std::uint32_t out_h) {
    int h = int(out_h) / 4;
    int lo = 40;
    if (h < lo) h = lo;
    if (h > int(out_h)) h = int(out_h);
    return h;
}

struct ConsoleGeom {
    bool active = false;
    int x0 = 0, y0 = 0, w = 0, h = 0;
};
ConsoleGeom console_geom(std::uint32_t out_w, std::uint32_t out_h, const ConsolePanel* c) {
    ConsoleGeom g;
    if (!c || !c->ok() || out_w == 0 || out_h == 0) return g;
    const int dst_w = int(out_w);
    int dst_h = int(std::int64_t(dst_w) * c->th / c->tw);
    if (dst_h < 1) dst_h = 1;
    if (dst_h > int(out_h)) dst_h = int(out_h);
    g.active = true;
    g.x0 = 0;
    g.w = dst_w;
    g.y0 = int(out_h) - dst_h;
    g.h = dst_h;
    return g;
}

void blend(std::vector<std::uint8_t>& img, std::uint32_t w, std::uint32_t h, int x, int y,
           std::uint8_t r, std::uint8_t g, std::uint8_t b, float a) {
    if (x < 0 || y < 0 || x >= int(w) || y >= int(h) || a <= 0.0f) return;
    if (a > 1.0f) a = 1.0f;
    std::size_t i = (std::size_t(y) * w + x) * 4;
    float da = img[i + 3] / 255.0f;
    float oa = a + da * (1.0f - a);
    if (oa <= 0.0f) return;
    auto comp = [&](std::uint8_t sc, std::uint8_t dc) {
        float v = (sc * a + dc * da * (1.0f - a)) / oa;
        if (v < 0.0f) v = 0.0f;
        else if (v > 255.0f) v = 255.0f;
        return std::uint8_t(v + 0.5f);
    };
    img[i]     = comp(r, img[i]);
    img[i + 1] = comp(g, img[i + 1]);
    img[i + 2] = comp(b, img[i + 2]);
    img[i + 3] = std::uint8_t(oa * 255.0f + 0.5f);
}

void fill_rect(std::vector<std::uint8_t>& img, std::uint32_t w, std::uint32_t h,
               int x0, int y0, int x1, int y1,
               std::uint8_t r, std::uint8_t g, std::uint8_t b, float a) {
    for (int y = y0; y < y1; ++y)
        for (int x = x0; x < x1; ++x) blend(img, w, h, x, y, r, g, b, a);
}

void stroke_rect(std::vector<std::uint8_t>& img, std::uint32_t w, std::uint32_t h,
                 int x0, int y0, int x1, int y1,
                 std::uint8_t r, std::uint8_t g, std::uint8_t b, float a) {
    if (x1 <= x0 || y1 <= y0) return;
    for (int x = x0; x < x1; ++x) {
        blend(img, w, h, x, y0, r, g, b, a);
        blend(img, w, h, x, y1 - 1, r, g, b, a);
    }
    for (int y = y0; y < y1; ++y) {
        blend(img, w, h, x0, y, r, g, b, a);
        blend(img, w, h, x1 - 1, y, r, g, b, a);
    }
}

void draw_line(std::vector<std::uint8_t>& img, std::uint32_t w, std::uint32_t h, int x0, int y0,
               int x1, int y1, int cx0, int cy0, int cx1, int cy1, std::uint8_t r,
               std::uint8_t g, std::uint8_t b, float a) {
    int dx = std::abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = -std::abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    for (int guard = 0; guard < 4096; ++guard) {
        if (x0 >= cx0 && x0 < cx1 && y0 >= cy0 && y0 < cy1) blend(img, w, h, x0, y0, r, g, b, a);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

int draw_text(std::vector<std::uint8_t>& img, std::uint32_t w, std::uint32_t h, int x, int y,
              const std::string& text, int scale, std::uint8_t r, std::uint8_t g,
              std::uint8_t b, int cx0, int cy0, int cx1, int cy1) {
    const int advance = (ui::kFontW + 1) * scale;
    int gx = x;
    for (char ch : text) {
        if (gx >= cx1) break;
        for (int ry = 0; ry < ui::kFontH; ++ry)
            for (int rx = 0; rx < ui::kFontW; ++rx) {
                if (!ui::font_pixel(ch, rx, ry)) continue;
                for (int sy = 0; sy < scale; ++sy)
                    for (int sx = 0; sx < scale; ++sx) {
                        int px = gx + rx * scale + sx;
                        int py = y + ry * scale + sy;
                        if (px < cx0 || px >= cx1 || py < cy0 || py >= cy1) continue;
                        blend(img, w, h, px, py, r, g, b, 1.0f);
                    }
            }
        gx += advance;
    }
    return gx;
}

}

std::vector<std::uint8_t> hud_bake_minimap_terrain(const std::vector<std::uint8_t>& level,
                                                   const std::vector<std::uint8_t>& blocked,
                                                   int gw, int gh, int out_w, int out_h) {
    std::vector<std::uint8_t> out;
    if (gw <= 0 || gh <= 0 || out_w <= 0 || out_h <= 0) return out;
    if (level.size() != std::size_t(gw) * std::size_t(gh)) return out;
    const bool have_block = blocked.size() == std::size_t(gw) * std::size_t(gh);

    int max_level = 0;
    for (std::uint8_t l : level)
        if (int(l) > max_level) max_level = int(l);

    out.resize(std::size_t(out_w) * std::size_t(out_h) * 3);
    for (int ty = 0; ty < out_h; ++ty) {
        const int gy = int(std::int64_t(ty) * gh / out_h);
        for (int tx = 0; tx < out_w; ++tx) {
            const int gx = int(std::int64_t(tx) * gw / out_w);
            const std::size_t gi = std::size_t(gy) * std::size_t(gw) + std::size_t(gx);
            std::uint8_t r, g, b;
            if (have_block && blocked[gi] != 0) {
                r = 58; g = 58; b = 66;
            } else {
                const float t = max_level > 0 ? float(level[gi]) / float(max_level) : 0.0f;
                r = std::uint8_t(38.0f + t * 90.0f + 0.5f);
                g = std::uint8_t(66.0f + t * 52.0f + 0.5f);
                b = std::uint8_t(40.0f + t * 44.0f + 0.5f);
            }
            const std::size_t oi = (std::size_t(ty) * std::size_t(out_w) + std::size_t(tx)) * 3;
            out[oi] = r; out[oi + 1] = g; out[oi + 2] = b;
        }
    }
    return out;
}

float hud_minimap_fog_factor(int vis_state) {
    switch (vis_state) {
        case 0: return 0.06f;
        case 1: return 0.45f;
        case 2: default: return 1.0f;
    }
}

int hud_panel_height(std::uint32_t out_h) { return panel_height(out_h); }

int hud_panel_height(std::uint32_t out_w, std::uint32_t out_h, const ConsolePanel* console) {
    const ConsoleGeom g = console_geom(out_w, out_h, console);
    if (g.active) return g.h;
    return panel_height(out_h);
}

bool hud_covers_point(std::uint32_t out_w, std::uint32_t out_h, const ConsolePanel* console,
                      float x, float y) {
    if (out_w == 0 || out_h == 0) return false;
    const ConsoleGeom g = console_geom(out_w, out_h, console);
    if (!g.active)
        return y >= float(int(out_h) - panel_height(out_h));
    if (x < float(g.x0) || x >= float(g.x0 + g.w) || y < float(g.y0) || y >= float(g.y0 + g.h))
        return false;
    int sx = int((x - float(g.x0)) * float(console->tw) / float(g.w));
    int sy = int((y - float(g.y0)) * float(console->th) / float(g.h));
    if (sx < 0) sx = 0;
    if (sy < 0) sy = 0;
    if (sx >= console->tw) sx = console->tw - 1;
    if (sy >= console->th) sy = console->th - 1;
    const std::size_t i = (std::size_t(sy) * std::size_t(console->tw) + std::size_t(sx)) * 4 + 3;
    return i < console->rgba->size() && (*console->rgba)[i] != 0;
}

bool authored_rect(const ConsolePanel* c, const HudAuthoredRects::R& pr, std::uint32_t out_h,
                   HudRect& out) {
    if (!c || !c->authored || !c->authored->ok || !pr.ok() || out_h == 0) return false;
    const double s = double(out_h) / double(kRefHeight);
    out.x0 = int(pr.x0 * s + 0.5);
    out.y0 = int(pr.y0 * s + 0.5);
    out.x1 = int(pr.x1 * s + 0.5);
    out.y1 = int(pr.y1 * s + 0.5);
    if (out.x1 <= out.x0) out.x1 = out.x0 + 1;
    if (out.y1 <= out.y0) out.y1 = out.y0 + 1;
    return true;
}

HudRect hud_minimap_rect(std::uint32_t out_w, std::uint32_t out_h, const ConsolePanel* console) {
    if (HudRect a; console && console->authored &&
                   authored_rect(console, console->authored->minimap, out_h, a))
        return a;
    (void)out_w;
    const int ph = panel_height(out_h);
    int side = ph - 2 * kPad;
    if (side < 1) side = 1;
    HudRect r;
    r.x0 = kPad;
    r.y0 = int(out_h) - ph + kPad;
    r.x1 = r.x0 + side;
    r.y1 = r.y0 + side;
    return r;
}

HudRect hud_unit_panel_rect(std::uint32_t out_w, std::uint32_t out_h, const ConsolePanel* console) {
    if (HudRect a; console && console->authored &&
                   authored_rect(console, console->authored->info, out_h, a))
        return a;
    const HudRect mm = hud_minimap_rect(out_w, out_h, nullptr);
    const int ph = panel_height(out_h);
    HudRect r;
    r.x0 = mm.x1 + 100;
    r.y0 = int(out_h) - ph + 12;
    r.x1 = r.x0 + 96;
    r.y1 = r.y0 + (ph - 20);
    return r;
}

HudRect hud_command_cell(std::uint32_t out_w, std::uint32_t out_h, int index,
                         const ConsolePanel* console) {
    HudRect r;
    if (index < 0 || index >= kCommandCols * kCommandRows) return r;
    const int cc = index % kCommandCols, cr = index / kCommandCols;

    if (console && console->authored && console->authored->buttons_ok) {
        if (HudRect a; authored_rect(console, console->authored->buttons[index], out_h, a))
            return a;
    }
    HudRect grid;
    if (console && console->authored && authored_rect(console, console->authored->command, out_h, grid)) {
        const int cw = grid.w() / kCommandCols;
        const int ch = grid.h() / kCommandRows;
        const int gap = std::max(1, cw / 14);
        r.x0 = grid.x0 + cc * cw + gap;
        r.y0 = grid.y0 + cr * ch + gap;
        r.x1 = grid.x0 + (cc + 1) * cw - gap;
        r.y1 = grid.y0 + (cr + 1) * ch - gap;
        if (r.x1 <= r.x0) r.x1 = r.x0 + 1;
        if (r.y1 <= r.y0) r.y1 = r.y0 + 1;
        return r;
    }

    const int W = int(out_w);
    const int ph = panel_height(out_h);
    const int panel_y0 = int(out_h) - ph;
    const int gap = 3;
    int cell = (ph - 2 * kPad - (kCommandRows - 1) * gap) / kCommandRows;
    if (cell < 8) cell = 8;
    const int grid_w = kCommandCols * cell + (kCommandCols - 1) * gap;
    const int x0 = W - kPad - grid_w;
    const int y0 = panel_y0 + kPad;
    r.x0 = x0 + cc * (cell + gap);
    r.y0 = y0 + cr * (cell + gap);
    r.x1 = r.x0 + cell;
    r.y1 = r.y0 + cell;
    return r;
}

std::vector<std::uint8_t> HudRender::render(const HudContent& content, const sim::World& world,
                                            float map_w, float map_h, std::uint32_t out_w,
                                            std::uint32_t out_h, const CameraFootprint* cam,
                                            const MinimapBackdrop* backdrop,
                                            const ConsolePanel* console) {
    std::vector<std::uint8_t> img(std::size_t(out_w) * out_h * 4, 0);
    if (out_w == 0 || out_h == 0) return img;

    const int W = int(out_w), H = int(out_h);
    const int ph = hud_panel_height(out_w, out_h, console);
    const int panel_y0 = H - ph;

    if (console && console->ok()) {
        composite_console(img, out_w, out_h, console->rgba->data(), console->tw, console->th);
    } else {
        fill_rect(img, out_w, out_h, 0, panel_y0, W, H, 14, 16, 22, 0.72f);
        for (int x = 0; x < W; ++x) blend(img, out_w, out_h, x, panel_y0, 90, 120, 160, 0.85f);
    }

    const std::string res = "MIN " + std::to_string(content.minerals) + "   GAS " +
                            std::to_string(content.gas) + "   SUP " +
                            std::to_string(content.supply_used) + "/" +
                            std::to_string(content.supply_max);
    const int res_w = int(res.size()) * (ui::kFontW + 1) * 2;
    HudRect rp;
    if (console && console->authored &&
        authored_rect(console, console->authored->resource, out_h, rp)) {
        const int tx0 = std::max(rp.x0, rp.x1 - res_w - 12);
        const int ty = rp.y0 + std::max(0, (rp.h() - ui::kFontH * 2) / 2);
        draw_text(img, out_w, out_h, tx0 + 6, ty, res, 2, 235, 236, 210, tx0, rp.y0, rp.x1,
                  rp.y1);
    } else {
        const int strip_h = ui::kFontH * 2 + 8;
        const int res_x0 = W - kPad - res_w - 8;
        fill_rect(img, out_w, out_h, res_x0, 0, W, strip_h, 14, 16, 22, 0.55f);
        draw_text(img, out_w, out_h, res_x0 + 4, 4, res, 2, 235, 236, 210, res_x0, 0, W,
                  strip_h);
    }

    const HudRect mm = hud_minimap_rect(out_w, out_h, console);
    const bool have_terrain = backdrop && backdrop->terrain && backdrop->tw > 0 &&
                              backdrop->th > 0 &&
                              backdrop->terrain->size() >=
                                  std::size_t(backdrop->tw) * std::size_t(backdrop->th) * 3;
    const bool have_fog = backdrop && backdrop->fog && backdrop->fw > 0 && backdrop->fh > 0 &&
                          backdrop->fog->size() >=
                              std::size_t(backdrop->fw) * std::size_t(backdrop->fh);
    if (have_terrain && mm.w() > 1 && mm.h() > 1) {
        const int mw = mm.w(), mh = mm.h();
        for (int py = 0; py < mh; ++py) {
            for (int px = 0; px < mw; ++px) {
                const float fx = float(px) / float(mw - 1);
                const float fy = float(py) / float(mh - 1);
                int sx = int(fx * float(backdrop->tw));
                int sy = int(fy * float(backdrop->th));
                if (sx >= backdrop->tw) sx = backdrop->tw - 1;
                if (sy >= backdrop->th) sy = backdrop->th - 1;
                const std::size_t si =
                    (std::size_t(sy) * std::size_t(backdrop->tw) + std::size_t(sx)) * 3;
                float r = (*backdrop->terrain)[si];
                float g = (*backdrop->terrain)[si + 1];
                float b = (*backdrop->terrain)[si + 2];
                if (have_fog) {
                    int cx = int(fx * float(backdrop->fw));
                    int cy = int(fy * float(backdrop->fh));
                    if (cx >= backdrop->fw) cx = backdrop->fw - 1;
                    if (cy >= backdrop->fh) cy = backdrop->fh - 1;
                    const float f = hud_minimap_fog_factor(
                        (*backdrop->fog)[std::size_t(cy) * std::size_t(backdrop->fw) +
                                         std::size_t(cx)]);
                    r *= f; g *= f; b *= f;
                }
                blend(img, out_w, out_h, mm.x0 + px, mm.y0 + py, std::uint8_t(r + 0.5f),
                      std::uint8_t(g + 0.5f), std::uint8_t(b + 0.5f), 1.0f);
            }
        }
    } else {
        fill_rect(img, out_w, out_h, mm.x0, mm.y0, mm.x1, mm.y1, 8, 10, 14, 0.92f);
    }
    stroke_rect(img, out_w, out_h, mm.x0, mm.y0, mm.x1, mm.y1, 70, 90, 120, 0.9f);
    if (map_w > 0.0f && map_h > 0.0f && mm.w() > 1 && mm.h() > 1) {
        for (const sim::Unit& u : world.units()) {
            if (!u.alive) continue;
            float fx = float(u.x.to_double()) / map_w;
            float fy = float(u.y.to_double()) / map_h;
            if (fx < 0.0f) fx = 0.0f;
            else if (fx > 1.0f) fx = 1.0f;
            if (fy < 0.0f) fy = 0.0f;
            else if (fy > 1.0f) fy = 1.0f;
            if (have_fog && backdrop->viewer_player >= 0 &&
                int(u.player) != backdrop->viewer_player) {
                int cx = int(fx * float(backdrop->fw));
                int cy = int(fy * float(backdrop->fh));
                if (cx >= backdrop->fw) cx = backdrop->fw - 1;
                if (cy >= backdrop->fh) cy = backdrop->fh - 1;
                if (cx < 0) cx = 0;
                if (cy < 0) cy = 0;
                const std::uint8_t code =
                    (*backdrop->fog)[std::size_t(cy) * std::size_t(backdrop->fw) + std::size_t(cx)];
                if (code != 2) continue;
            }
            const int bx = mm.x0 + int(fx * float(mm.w() - 1));
            const int by = mm.y0 + int(fy * float(mm.h() - 1));
            std::uint8_t r, g, b;
            if (u.player == 0) { r = 230; g = 40; b = 40; }
            else if (u.player == 1) { r = 60; g = 90; b = 230; }
            else { r = 150; g = 150; b = 150; }
            for (int dy = 0; dy < 2; ++dy)
                for (int dx = 0; dx < 2; ++dx) {
                    const int px = bx + dx, py = by + dy;
                    if (px >= mm.x0 && px < mm.x1 && py >= mm.y0 && py < mm.y1)
                        blend(img, out_w, out_h, px, py, r, g, b, 1.0f);
                }
        }

        if (cam && cam->valid) {
            int px[4], py[4];
            for (int k = 0; k < 4; ++k) {
                px[k] = mm.x0 + int(cam->x[k] / map_w * float(mm.w() - 1));
                py[k] = mm.y0 + int(cam->y[k] / map_h * float(mm.h() - 1));
            }
            for (int k = 0; k < 4; ++k)
                draw_line(img, out_w, out_h, px[k], py[k], px[(k + 1) % 4], py[(k + 1) % 4],
                          mm.x0, mm.y0, mm.x1, mm.y1, 240, 240, 240, 0.95f);
        }
    }

    HudRect ip;
    if (console && console->authored &&
        authored_rect(console, console->authored->info, out_h, ip)) {
        draw_text(img, out_w, out_h, ip.x0 + kPad * 2, ip.y0 + kPad * 2,
                  content.selection_summary, 2, 220, 224, 232, ip.x0, ip.y0, ip.x1, ip.y1);
    } else {
        const int info_x = mm.x1 + kPad * 2;
        draw_text(img, out_w, out_h, info_x, mm.y0, content.selection_summary, 2, 220, 224, 232,
                  info_x, mm.y0, W, H);
    }

    {
        int err_y = panel_y0 - ui::kFontH * 2 - 8;
        int ul_y = kPad + 2;
        const int adv = (ui::kFontW + 1) * 2;
        for (const HudMessageLine& m : content.messages) {
            const int tw = int(m.text.size()) * adv;
            if (m.zone == MsgZone::Error) {
                int x = (W - tw) / 2;
                if (x < kPad) x = kPad;
                fill_rect(img, out_w, out_h, x - 4, err_y - 2, x + tw + 4,
                          err_y + ui::kFontH * 2 + 2, 60, 12, 12, 0.65f);
                draw_text(img, out_w, out_h, x, err_y, m.text, 2, 255, 130, 130, 0, 0, W, H);
                err_y -= ui::kFontH * 2 + 6;
            } else {
                std::uint8_t r = 235, g = 225, b = 130;
                if (m.zone == MsgZone::Transmission) {
                    r = 150;
                    g = 210;
                    b = 240;
                }
                draw_text(img, out_w, out_h, kPad, ul_y, m.text, 2, r, g, b, 0, 0, W, H);
                ul_y += ui::kFontH * 2 + 4;
            }
        }
    }

    return img;
}

}
