#include "omc/core/env_once.hpp"
#include <cstring>
#include "omc/render/frame_prof.hpp"
#include "omc/app/hud_render.hpp"

#include "omc/app/overlay_draw.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <string>

#include "omc/app/console_panel.hpp"
#include "omc/sim/world.hpp"
#include "omc/ui/font.hpp"

namespace omc::app {

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
    std::size_t i = (std::size_t(y) * w + x) * 4;
    if (a >= 1.0f) {
        img[i] = r;
        img[i + 1] = g;
        img[i + 2] = b;
        img[i + 3] = 255;
        return;
    }
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
    return draw::text(img, w, h, x, y, text, scale, r, g, b, cx0, cy0, cx1, cy1);
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
        case 1: return 0.31f;
        case 2: default: return 1.0f;
    }
}

std::uint8_t hud_fog_veil_alpha(int vis_state) {
    switch (vis_state) {
        case 0: return 255;
        case 1: return 165;
        case 2: default: return 0;
    }
}

namespace {
float bilinear_clamped(const std::vector<std::uint8_t>& field, int w, int h, float fx, float fy) {
    if (fx < 0.0f) fx = 0.0f;
    if (fy < 0.0f) fy = 0.0f;
    const float max_x = float(w - 1), max_y = float(h - 1);
    if (fx > max_x) fx = max_x;
    if (fy > max_y) fy = max_y;
    const int x0 = int(fx), y0 = int(fy);
    const int x1 = x0 + 1 < w ? x0 + 1 : x0;
    const int y1 = y0 + 1 < h ? y0 + 1 : y0;
    const float tx = fx - float(x0), ty = fy - float(y0);
    const float a00 = float(field[std::size_t(y0) * std::size_t(w) + std::size_t(x0)]);
    const float a10 = float(field[std::size_t(y0) * std::size_t(w) + std::size_t(x1)]);
    const float a01 = float(field[std::size_t(y1) * std::size_t(w) + std::size_t(x0)]);
    const float a11 = float(field[std::size_t(y1) * std::size_t(w) + std::size_t(x1)]);
    return (a00 + (a10 - a00) * tx) + ((a01 + (a11 - a01) * tx) - (a00 + (a10 - a00) * tx)) * ty;
}
}

std::vector<std::uint8_t> hud_fog_veil_cells(const std::vector<std::uint8_t>& codes, int cw,
                                             int ch) {
    std::vector<std::uint8_t> out;
    if (cw <= 0 || ch <= 0 || codes.size() < std::size_t(cw) * std::size_t(ch)) return out;
    out.resize(std::size_t(cw) * std::size_t(ch));
    for (std::size_t i = 0; i < out.size(); ++i) out[i] = hud_fog_veil_alpha(int(codes[i]));
    return out;
}

float hud_fog_veil_alpha_at(const std::vector<std::uint8_t>& cell_alpha, int cw, int ch, float cell,
                            float origin_x, float origin_y, float wx, float wy) {
    if (cw <= 0 || ch <= 0 || cell <= 0.0f ||
        cell_alpha.size() < std::size_t(cw) * std::size_t(ch))
        return 0.0f;
    return bilinear_clamped(cell_alpha, cw, ch, (wx - origin_x) / cell - 0.5f,
                            (wy - origin_y) / cell - 0.5f);
}

void hud_fog_veil_blit(std::vector<std::uint8_t>& rgba, std::uint32_t out_w, std::uint32_t out_h,
                       const std::vector<std::uint8_t>& block_alpha, int bw, int bh, int step,
                       const std::vector<std::uint8_t>* skip) {
    if (out_w == 0 || out_h == 0 || bw <= 0 || bh <= 0 || step <= 0) return;
    if (block_alpha.size() < std::size_t(bw) * std::size_t(bh)) return;
    if (rgba.size() < std::size_t(out_w) * std::size_t(out_h) * 4) return;
    const float half = float(step) * 0.5f;
    for (std::uint32_t y = 0; y < out_h; ++y) {
        const float fy = (float(y) - half) / float(step);
        int nby = int((float(y) + 0.5f) / float(step));
        if (nby < 0) nby = 0;
        if (nby >= bh) nby = bh - 1;
        for (std::uint32_t x = 0; x < out_w; ++x) {
            int nbx = int((float(x) + 0.5f) / float(step));
            if (nbx < 0) nbx = 0;
            if (nbx >= bw) nbx = bw - 1;
            const std::size_t bi = std::size_t(nby) * std::size_t(bw) + std::size_t(nbx);
            if (skip && skip->size() > bi && (*skip)[bi]) continue;
            const float fx = (float(x) - half) / float(step);
            const float a = bilinear_clamped(block_alpha, bw, bh, fx, fy);
            if (a < 0.5f) continue;
            const std::size_t pi = (std::size_t(y) * std::size_t(out_w) + std::size_t(x)) * 4;
            const std::uint8_t q = std::uint8_t(a > 254.5f ? 255 : int(a + 0.5f));
            rgba[pi] = 0;
            rgba[pi + 1] = 0;
            rgba[pi + 2] = 0;
            if (rgba[pi + 3] < q) rgba[pi + 3] = q;
        }
    }
}

bool hud_fog_body_drawn(int vis_state, FogVisibility fv) {
    if (vis_state == 2) return true;
    if (vis_state == 0) return false;
    return fv != FogVisibility::Visible;
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

HudRect hud_equipment_cell(std::uint32_t out_w, std::uint32_t out_h, int index,
                           const ConsolePanel* console, int count) {
    (void)out_w;
    HudRect ep{0, 0, 0, 0};
    if (index < 0 || !console || !console->authored) return HudRect{0, 0, 0, 0};
    if (count > 0 && index < count && console->authored->ok && console->authored->info.ok() && out_h > 0 &&
        !(console->authored->equipment.ok() && console->authored->equipment.x1 - console->authored->equipment.x0 > 8.0f)) {
        const double s = double(out_h) / double(kRefHeight);
        const double mid = double(console->authored->info.x1) - 201.0;
        const double top = double(console->authored->info.y0) + 108.0;
        const double x0 = mid - 32.0 * double(count) + 64.0 * double(index);
        return HudRect{int(x0 * s + 0.5), int(top * s + 0.5), int((x0 + 64.0) * s + 0.5), int((top + 64.0) * s + 0.5)};
    }
    if (!authored_rect(console, console->authored->equipment, out_h, ep) || ep.w() <= 8 || ep.h() <= 8)
        return HudRect{0, 0, 0, 0};
    const int cell = std::min(ep.h(), 64 * int(out_h) / int(kRefHeight) > 8 ? 64 * int(out_h) / int(kRefHeight) : ep.h());
    const int x = ep.x0 + index * (cell + 2);
    if (x + cell > ep.x1) return HudRect{0, 0, 0, 0};
    return HudRect{x, ep.y0, x + cell, ep.y0 + cell};
}

HudRect hud_cargo_slot(std::uint32_t out_w, std::uint32_t out_h, int slot, const ConsolePanel* console) {
    (void)out_w;
    if (slot < 0 || slot >= 8 || !console || !console->authored || !console->authored->cargo_slots_ok)
        return HudRect{0, 0, 0, 0};
    if (HudRect a; authored_rect(console, console->authored->cargo_slots[slot], out_h, a)) return a;
    return HudRect{0, 0, 0, 0};
}

HudRect hud_queue_slot(std::uint32_t out_w, std::uint32_t out_h, int slot, const ConsolePanel* console) {
    (void)out_w;
    if (slot >= 0 && slot < 8 && console && console->authored && console->authored->ok && console->authored->info.ok() &&
        out_h > 0 && !std::getenv("OMC_QUEUE_SLOTS_SOLVED")) {
        const double s = double(out_h) / double(kRefHeight);
        const double left = double(console->authored->info.x1) - 201.0 - (4.0 * 64.0 + 3.0 * 2.0) * 0.5;
        const double top = double(console->authored->info.y0) + 63.0;
        const double x0 = slot == 0 ? left : left + double(slot - 1) * 66.0;
        const double y0 = slot == 0 ? top : top + 66.0;
        return HudRect{int(x0 * s + 0.5), int(y0 * s + 0.5), int((x0 + 64.0) * s + 0.5), int((y0 + 64.0) * s + 0.5)};
    }
    if (slot < 0 || slot >= 8 || !console || !console->authored || !console->authored->queue_slots_ok)
        return HudRect{0, 0, 0, 0};
    if (HudRect a; authored_rect(console, console->authored->queue_slots[slot], out_h, a)) return a;
    return HudRect{0, 0, 0, 0};
}

HudRect hud_behavior_slot(std::uint32_t out_w, std::uint32_t out_h, int slot, const ConsolePanel* console) {
    (void)out_w;
    if (slot < 0 || slot >= 6 || !console || !console->authored || !console->authored->behavior_slots_ok)
        return HudRect{0, 0, 0, 0};
    if (HudRect a; authored_rect(console, console->authored->behavior_slots[slot], out_h, a)) return a;
    return HudRect{0, 0, 0, 0};
}

HudRect hud_idle_button_rect(std::uint32_t out_w, std::uint32_t out_h, const ConsolePanel* console) {
    (void)out_w;
    if (HudRect a; console && console->authored &&
                   authored_rect(console, console->authored->idle_button, out_h, a))
        return a;
    return HudRect{0, 0, 0, 0};
}

HudRect hud_warp_button_rect(std::uint32_t out_w, std::uint32_t out_h, const ConsolePanel* console) {
    (void)out_w;
    if (HudRect a; console && console->authored &&
                   authored_rect(console, console->authored->warp_button, out_h, a))
        return a;
    return HudRect{0, 0, 0, 0};
}

HudRect hud_army_button_rect(std::uint32_t out_w, std::uint32_t out_h, const ConsolePanel* console) {
    (void)out_w;
    if (HudRect a; console && console->authored &&
                   authored_rect(console, console->authored->army_button, out_h, a))
        return a;
    return HudRect{0, 0, 0, 0};
}

HudRect hud_progress_bar_rect(std::uint32_t out_w, std::uint32_t out_h, const ConsolePanel* console) {
    (void)out_w;
    if (HudRect a; console && console->authored && console->authored->progress_ok &&
                   authored_rect(console, console->authored->progress_bar, out_h, a))
        return a;
    return HudRect{0, 0, 0, 0};
}

HudRect hud_progress_button_rect(std::uint32_t out_w, std::uint32_t out_h, const ConsolePanel* console) {
    (void)out_w;
    if (HudRect a; console && console->authored && console->authored->progress_ok &&
                   authored_rect(console, console->authored->progress_button, out_h, a))
        return a;
    return HudRect{0, 0, 0, 0};
}

HudRect hud_unit_wireframe_rect(std::uint32_t out_w, std::uint32_t out_h,
                                const ConsolePanel* console) {
    (void)out_w;
    if (HudRect a; console && console->authored &&
                   authored_rect(console, console->authored->wireframe, out_h, a))
        return a;
    return HudRect{0, 0, 0, 0};
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

HudRect hud_group_slot(std::uint32_t, std::uint32_t out_h, int index,
                       const ConsolePanel* console) {
    HudRect r;
    if (index < 0 || index >= kGroupSlots) return r;
    if (!console || !console->authored || !console->authored->group_slots_ok) return r;
    if (!authored_rect(console, console->authored->group_slots[index], out_h, r)) return HudRect{};
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

void hud_minimap_world_to_pixel(const HudRect& mm, const MinimapExtent& e, float wx, float wy, int& px, int& py) {
    float fx = e.ok() ? (wx - e.x0) / (e.x1 - e.x0) : 0.0f;
    float fy = e.ok() ? (wy - e.y0) / (e.y1 - e.y0) : 0.0f;
    fx = fx < 0.0f ? 0.0f : (fx > 1.0f ? 1.0f : fx);
    fy = fy < 0.0f ? 0.0f : (fy > 1.0f ? 1.0f : fy);
    fy = 1.0f - fy;
    px = mm.x0 + int(fx * float(mm.w() - 1) + 1e-3f);
    py = mm.y0 + int(fy * float(mm.h() - 1) + 1e-3f);
}

void hud_minimap_pixel_to_world(const HudRect& mm, const MinimapExtent& e, float sx, float sy, float& wx, float& wy) {
    float fx = (sx - float(mm.x0)) / float(mm.w() > 1 ? mm.w() - 1 : 1);
    float fy = (sy - float(mm.y0)) / float(mm.h() > 1 ? mm.h() - 1 : 1);
    fx = fx < 0.0f ? 0.0f : (fx > 1.0f ? 1.0f : fx);
    fy = fy < 0.0f ? 0.0f : (fy > 1.0f ? 1.0f : fy);
    fy = 1.0f - fy;
    wx = e.x0 + fx * (e.x1 - e.x0);
    wy = e.y0 + fy * (e.y1 - e.y0);
}

std::vector<std::uint8_t> HudRender::render(const HudContent& content, const sim::World& world,
                                            float map_w, float map_h, std::uint32_t out_w,
                                            std::uint32_t out_h, const CameraFootprint* cam,
                                            const MinimapBackdrop* backdrop,
                                            const ConsolePanel* console) {
    std::vector<std::uint8_t> img;
    render_into(img, content, world, map_w, map_h, out_w, out_h, cam, backdrop, console);
    return img;
}

void HudRender::render_into(std::vector<std::uint8_t>& img, const HudContent& content,
                            const sim::World& world, float map_w, float map_h,
                            std::uint32_t out_w, std::uint32_t out_h, const CameraFootprint* cam,
                            const MinimapBackdrop* backdrop, const ConsolePanel* console) {
    const std::size_t img_bytes = std::size_t(out_w) * out_h * 4;
    if (out_w == 0 || out_h == 0) {
        img.assign(img_bytes, 0);
        return;
    }

    const int W = int(out_w), H = int(out_h);
    const int ph = hud_panel_height(out_w, out_h, console);
    const int panel_y0 = H - ph;

    if (console && console->ok()) {
        static std::vector<std::uint8_t> s_base;
        static std::uint32_t s_w = 0, s_h = 0, s_tw = 0, s_th = 0;
        static const void* s_px = nullptr;
        static std::size_t s_sz = 0;
        const bool base_hit = s_px == static_cast<const void*>(console->rgba->data()) &&
                              s_sz == console->rgba->size() && s_w == out_w && s_h == out_h &&
                              s_tw == std::uint32_t(console->tw) && s_th == std::uint32_t(console->th) &&
                              s_base.size() == img_bytes;
        const auto t_cc0 = std::chrono::steady_clock::now();
        if (base_hit) {
            img.resize(img_bytes);
            std::memcpy(img.data(), s_base.data(), img_bytes);
        } else {
            img.assign(img_bytes, 0);
            {
                constexpr double kInfoStubTopFrac = (double(kRefHeight) - 232.0) / double(kRefHeight);
                const int band_y0 = int(double(H) * kInfoStubTopFrac + 0.5);
                if (band_y0 >= 0 && band_y0 < H) fill_rect(img, out_w, out_h, 0, band_y0, W, H, 14, 16, 22, 1.0f);
            }
            composite_console(img, out_w, out_h, console->rgba->data(), console->tw, console->th);
            s_base = img;
            s_px = static_cast<const void*>(console->rgba->data());
            s_sz = console->rgba->size();
            s_w = out_w; s_h = out_h;
            s_tw = std::uint32_t(console->tw); s_th = std::uint32_t(console->th);
        }
        render::frame_prof_add("hud.console_composite", std::chrono::duration_cast<std::chrono::duration<double, std::milli>>(std::chrono::steady_clock::now() - t_cc0).count());
    } else {
        img.assign(img_bytes, 0);
        fill_rect(img, out_w, out_h, 0, panel_y0, W, H, 14, 16, 22, 0.72f);
        for (int x = 0; x < W; ++x) blend(img, out_w, out_h, x, panel_y0, 90, 120, 160, 0.85f);
    }

    const std::string clock = hud_clock_text(content.game_seconds);
    if (!clock.empty() && console && console->ok() && !content.widget_art) {
        const double pt = double(out_h) / double(kRefHeight);
        const int cx = int(306.3 * pt + 0.5), cy = int(867.4 * pt + 0.5);
        const int scale = std::max(1, int(35.4 * pt / double(ui::kFontH) + 0.5));
        const int tw = draw::text_w(clock, scale), th = ui::kFontH * scale;
        fill_rect(img, out_w, out_h, cx - 6 * scale, cy - 2 * scale, cx + tw + 4 * scale, cy + th + 2 * scale,
                  6, 8, 9, 0.92f);
        draw_text(img, out_w, out_h, cx, cy, clock, scale, 143, 234, 174, 0, 0, int(out_w), int(out_h));
    }
    const std::string res_head = ((clock.empty() || (console && console->ok())) ? std::string() : clock + "   ") +
                                 "MIN " + std::to_string(content.minerals) + "   GAS " +
                                 std::to_string(content.gas) + "   ";
    const std::string res_sup = "SUP " + std::to_string(content.supply_used) + "/" +
                                std::to_string(content.supply_max);
    const std::string res = res_head + res_sup;
    const bool supply_blocked = hud_supply_blocked(content.supply_used, content.supply_max);
    const int res_w = draw::text_w(res, 2);
    HudRect rp;
    if (console && console->authored &&
        authored_rect(console, console->authored->resource, out_h, rp)) {
        const HudAuthoredRects* au = console->authored;
        if (au->resource_icons_ok) {
            const std::string vals[3] = {
                std::to_string(content.minerals), std::to_string(content.gas),
                std::to_string(content.supply_used) + "/" + std::to_string(content.supply_max)};
            const HudAuthoredRects::R* labs[3] = {&au->resource_label[0], &au->resource_label[1],
                                                  &au->supply_label};
            const bool blocked = hud_supply_blocked(content.supply_used, content.supply_max);
            for (int i = 0; i < 3; ++i) {
                HudRect lr;
                if (!authored_rect(console, *labs[i], out_h, lr)) continue;
                const int ly = lr.y0 + std::max(0, (lr.h() - ui::kFontH * 2) / 2);
                const bool red = blocked && i == 2;
                static const bool cream = std::getenv("OMC_RESOURCE_ICON_PLATES") != nullptr;
                draw_text(img, out_w, out_h, lr.x0 + 2, ly, vals[i], 2,
                          red ? kSupplyBlockedR : (cream ? 235 : 255), red ? kSupplyBlockedG : (cream ? 236 : 255),
                          red ? kSupplyBlockedB : (cream ? 210 : 255), lr.x0, lr.y0, lr.x1, lr.y1);
            }
        } else {
            const int tx0 = std::max(rp.x0, rp.x1 - res_w - 12);
            const int ty = rp.y0 + std::max(0, (rp.h() - ui::kFontH * 2) / 2);
            const int pen = draw_text(img, out_w, out_h, tx0 + 6, ty, res_head, 2, 235, 236, 210,
                                      tx0, rp.y0, rp.x1, rp.y1);
            draw_text(img, out_w, out_h, pen, ty, res_sup, 2,
                      supply_blocked ? kSupplyBlockedR : 235, supply_blocked ? kSupplyBlockedG : 236,
                      supply_blocked ? kSupplyBlockedB : 210, tx0, rp.y0, rp.x1, rp.y1);
        }
    } else {
        const int strip_h = ui::kFontH * 2 + 8;
        const int res_x0 = W - kPad - res_w - 8;
        fill_rect(img, out_w, out_h, res_x0, 0, W, strip_h, 14, 16, 22, 0.55f);
        const int pen = draw_text(img, out_w, out_h, res_x0 + 4, 4, res_head, 2, 235, 236, 210,
                                  res_x0, 0, W, strip_h);
        draw_text(img, out_w, out_h, pen, 4, res_sup, 2,
                  supply_blocked ? kSupplyBlockedR : 235, supply_blocked ? kSupplyBlockedG : 236,
                  supply_blocked ? kSupplyBlockedB : 210, res_x0, 0, W, strip_h);
    }

    const HudRect mm = hud_minimap_rect(out_w, out_h, console);
    MinimapExtent mext;
    if (backdrop && backdrop->extent.ok()) mext = backdrop->extent;
    else { mext.x1 = map_w; mext.y1 = map_h; }
    const bool have_terrain = backdrop && backdrop->terrain && backdrop->tw > 0 &&
                              backdrop->th > 0 &&
                              backdrop->terrain->size() >=
                                  std::size_t(backdrop->tw) * std::size_t(backdrop->th) * 3;
    const bool have_fog = backdrop && backdrop->fog && backdrop->fw > 0 && backdrop->fh > 0 &&
                          backdrop->fog->size() >=
                              std::size_t(backdrop->fw) * std::size_t(backdrop->fh);
    const auto t_mm0 = std::chrono::steady_clock::now();
    static const bool mm_uncached = std::getenv("OMC_MINIMAP_TEXEL_UNCACHED") != nullptr;
    struct MmTexel { float r, g, b, ax, ay; int x0, y0, x1, y1; };
    struct MmCache {
        const void* terrain = nullptr;
        std::size_t tsize = 0, tprobe = 0;
        int tw = 0, th = 0, fw = 0, fh = 0, mw = 0, mh = 0;
        float ext[4] = {0, 0, 0, 0}, text[4] = {0, 0, 0, 0}, mapw = 0, maph = 0;
        bool fog = false, nearest = false;
        std::vector<MmTexel> t;
    };
    static MmCache mmc;
    if (have_terrain && mm.w() > 1 && mm.h() > 1 && !mm_uncached) {
        const int mw = mm.w(), mh = mm.h();
        static const bool mm_nearest_c = std::getenv("OMC_MINIMAP_NEAREST") != nullptr;
        const std::vector<std::uint8_t>& tv = *backdrop->terrain;
        std::size_t probe = 1469598103u;
        for (std::size_t i = 0; i < tv.size(); i += 997) probe = (probe ^ tv[i]) * 1099511628211ull;
        const MinimapExtent& te = backdrop->terrain_extent;
        const bool same = mmc.terrain == static_cast<const void*>(tv.data()) && mmc.tsize == tv.size() && mmc.tprobe == probe &&
                          mmc.tw == backdrop->tw && mmc.th == backdrop->th && mmc.mw == mw && mmc.mh == mh &&
                          mmc.fw == (have_fog ? backdrop->fw : 0) && mmc.fh == (have_fog ? backdrop->fh : 0) && mmc.fog == have_fog &&
                          mmc.nearest == mm_nearest_c && mmc.ext[0] == mext.x0 && mmc.ext[1] == mext.y0 && mmc.ext[2] == mext.x1 &&
                          mmc.ext[3] == mext.y1 && mmc.text[0] == te.x0 && mmc.text[1] == te.y0 && mmc.text[2] == te.x1 && mmc.text[3] == te.y1 &&
                          mmc.mapw == map_w && mmc.maph == map_h;
        if (!same) {
            mmc.terrain = tv.data();
            mmc.tsize = tv.size();
            mmc.tprobe = probe;
            mmc.tw = backdrop->tw;
            mmc.th = backdrop->th;
            mmc.mw = mw;
            mmc.mh = mh;
            mmc.fw = have_fog ? backdrop->fw : 0;
            mmc.fh = have_fog ? backdrop->fh : 0;
            mmc.fog = have_fog;
            mmc.nearest = mm_nearest_c;
            mmc.ext[0] = mext.x0; mmc.ext[1] = mext.y0; mmc.ext[2] = mext.x1; mmc.ext[3] = mext.y1;
            mmc.text[0] = te.x0; mmc.text[1] = te.y0; mmc.text[2] = te.x1; mmc.text[3] = te.y1;
            mmc.mapw = map_w;
            mmc.maph = map_h;
            mmc.t.assign(std::size_t(mw) * std::size_t(mh), MmTexel{});
            for (int py = 0; py < mh; ++py)
                for (int px = 0; px < mw; ++px) {
                    MmTexel& k = mmc.t[std::size_t(py) * std::size_t(mw) + std::size_t(px)];
                    float fx = float(px) / float(mw - 1), fy = 1.0f - float(py) / float(mh - 1);
                    if (map_w > 0.0f && map_h > 0.0f && mext.ok()) {
                        fx = (mext.x0 + fx * (mext.x1 - mext.x0)) / map_w;
                        fy = (mext.y0 + fy * (mext.y1 - mext.y0)) / map_h;
                        fx = fx < 0.0f ? 0.0f : (fx > 1.0f ? 1.0f : fx);
                        fy = fy < 0.0f ? 0.0f : (fy > 1.0f ? 1.0f : fy);
                    }
                    float tfx = fx, tfy = fy;
                    if (te.ok() && map_w > 0.0f && map_h > 0.0f) {
                        tfx = (fx * map_w - te.x0) / (te.x1 - te.x0);
                        tfy = (fy * map_h - te.y0) / (te.y1 - te.y0);
                        tfx = tfx < 0.0f ? 0.0f : (tfx > 1.0f ? 1.0f : tfx);
                        tfy = tfy < 0.0f ? 0.0f : (tfy > 1.0f ? 1.0f : tfy);
                    }
                    int sx = int(tfx * float(backdrop->tw));
                    int sy = int(tfy * float(backdrop->th));
                    if (sx >= backdrop->tw) sx = backdrop->tw - 1;
                    if (sy >= backdrop->th) sy = backdrop->th - 1;
                    const std::size_t si = (std::size_t(sy) * std::size_t(backdrop->tw) + std::size_t(sx)) * 3;
                    k.r = tv[si];
                    k.g = tv[si + 1];
                    k.b = tv[si + 2];
                    if (!mm_nearest_c) {
                        const float ux = tfx * float(backdrop->tw) - 0.5f, uy = tfy * float(backdrop->th) - 0.5f;
                        const int x0i = std::max(0, std::min(backdrop->tw - 1, int(std::floor(ux)))), y0i = std::max(0, std::min(backdrop->th - 1, int(std::floor(uy))));
                        const int x1i = std::min(backdrop->tw - 1, x0i + 1), y1i = std::min(backdrop->th - 1, y0i + 1);
                        const float ax = std::max(0.0f, std::min(1.0f, ux - float(x0i))), ay = std::max(0.0f, std::min(1.0f, uy - float(y0i)));
                        auto tx = [&](int x, int y, int c) { return float(tv[(std::size_t(y) * std::size_t(backdrop->tw) + std::size_t(x)) * 3 + std::size_t(c)]); };
                        float c[3];
                        for (int q = 0; q < 3; ++q)
                            c[q] = (tx(x0i, y0i, q) * (1.0f - ax) + tx(x1i, y0i, q) * ax) * (1.0f - ay) + (tx(x0i, y1i, q) * (1.0f - ax) + tx(x1i, y1i, q) * ax) * ay;
                        k.r = c[0]; k.g = c[1]; k.b = c[2];
                    }
                    k.x0 = k.y0 = k.x1 = k.y1 = 0;
                    k.ax = k.ay = 0.0f;
                    if (have_fog && !mm_nearest_c) {
                        const float ux = fx * float(backdrop->fw) - 0.5f, uy = fy * float(backdrop->fh) - 0.5f;
                        k.x0 = std::max(0, std::min(backdrop->fw - 1, int(std::floor(ux))));
                        k.y0 = std::max(0, std::min(backdrop->fh - 1, int(std::floor(uy))));
                        k.x1 = std::min(backdrop->fw - 1, k.x0 + 1);
                        k.y1 = std::min(backdrop->fh - 1, k.y0 + 1);
                        k.ax = std::max(0.0f, std::min(1.0f, ux - float(k.x0)));
                        k.ay = std::max(0.0f, std::min(1.0f, uy - float(k.y0)));
                    } else if (have_fog) {
                        int cx = int(fx * float(backdrop->fw));
                        int cy = int(fy * float(backdrop->fh));
                        if (cx >= backdrop->fw) cx = backdrop->fw - 1;
                        if (cy >= backdrop->fh) cy = backdrop->fh - 1;
                        k.x0 = cx;
                        k.y0 = cy;
                    }
                }
        }
        const std::vector<std::uint8_t>* fogv = have_fog ? backdrop->fog : nullptr;
        const std::size_t fwz = have_fog ? std::size_t(backdrop->fw) : 0;
        for (int py = 0; py < mh; ++py)
            for (int px = 0; px < mw; ++px) {
                const MmTexel& k = mmc.t[std::size_t(py) * std::size_t(mw) + std::size_t(px)];
                float r = k.r, g = k.g, b = k.b;
                if (fogv && !mm_nearest_c) {
                    auto ff = [&](int x, int y) { return hud_minimap_fog_factor((*fogv)[std::size_t(y) * fwz + std::size_t(x)]); };
                    const float f = (ff(k.x0, k.y0) * (1.0f - k.ax) + ff(k.x1, k.y0) * k.ax) * (1.0f - k.ay) + (ff(k.x0, k.y1) * (1.0f - k.ax) + ff(k.x1, k.y1) * k.ax) * k.ay;
                    r *= f; g *= f; b *= f;
                } else if (fogv) {
                    const float f = hud_minimap_fog_factor((*fogv)[std::size_t(k.y0) * fwz + std::size_t(k.x0)]);
                    r *= f; g *= f; b *= f;
                }
                blend(img, out_w, out_h, mm.x0 + px, mm.y0 + py, std::uint8_t(r + 0.5f), std::uint8_t(g + 0.5f), std::uint8_t(b + 0.5f), 1.0f);
            }
        render::frame_prof_add("hud.minimap_terrain", std::chrono::duration_cast<std::chrono::duration<double, std::milli>>(std::chrono::steady_clock::now() - t_mm0).count());
    } else if (have_terrain && mm.w() > 1 && mm.h() > 1) {
        const int mw = mm.w(), mh = mm.h();
        for (int py = 0; py < mh; ++py) {
            for (int px = 0; px < mw; ++px) {
                float fx = float(px) / float(mw - 1), fy = 1.0f - float(py) / float(mh - 1);
                if (map_w > 0.0f && map_h > 0.0f && mext.ok()) {
                    fx = (mext.x0 + fx * (mext.x1 - mext.x0)) / map_w;
                    fy = (mext.y0 + fy * (mext.y1 - mext.y0)) / map_h;
                    fx = fx < 0.0f ? 0.0f : (fx > 1.0f ? 1.0f : fx);
                    fy = fy < 0.0f ? 0.0f : (fy > 1.0f ? 1.0f : fy);
                }
                float tfx = fx, tfy = fy;
                if (backdrop->terrain_extent.ok() && map_w > 0.0f && map_h > 0.0f) {
                    const MinimapExtent& te = backdrop->terrain_extent;
                    tfx = (fx * map_w - te.x0) / (te.x1 - te.x0);
                    tfy = (fy * map_h - te.y0) / (te.y1 - te.y0);
                    tfx = tfx < 0.0f ? 0.0f : (tfx > 1.0f ? 1.0f : tfx);
                    tfy = tfy < 0.0f ? 0.0f : (tfy > 1.0f ? 1.0f : tfy);
                }
                int sx = int(tfx * float(backdrop->tw));
                int sy = int(tfy * float(backdrop->th));
                if (sx >= backdrop->tw) sx = backdrop->tw - 1;
                if (sy >= backdrop->th) sy = backdrop->th - 1;
                const std::size_t si =
                    (std::size_t(sy) * std::size_t(backdrop->tw) + std::size_t(sx)) * 3;
                float r = (*backdrop->terrain)[si];
                float g = (*backdrop->terrain)[si + 1];
                float b = (*backdrop->terrain)[si + 2];
                static const bool mm_nearest = std::getenv("OMC_MINIMAP_NEAREST") != nullptr;
                if (!mm_nearest) {
                    const float ux = tfx * float(backdrop->tw) - 0.5f, uy = tfy * float(backdrop->th) - 0.5f;
                    const int x0i = std::max(0, std::min(backdrop->tw - 1, int(std::floor(ux)))), y0i = std::max(0, std::min(backdrop->th - 1, int(std::floor(uy))));
                    const int x1i = std::min(backdrop->tw - 1, x0i + 1), y1i = std::min(backdrop->th - 1, y0i + 1);
                    const float ax = std::max(0.0f, std::min(1.0f, ux - float(x0i))), ay = std::max(0.0f, std::min(1.0f, uy - float(y0i)));
                    auto tx = [&](int x, int y, int k) { return float((*backdrop->terrain)[(std::size_t(y) * std::size_t(backdrop->tw) + std::size_t(x)) * 3 + std::size_t(k)]); };
                    float c[3];
                    for (int k = 0; k < 3; ++k)
                        c[k] = (tx(x0i, y0i, k) * (1.0f - ax) + tx(x1i, y0i, k) * ax) * (1.0f - ay) + (tx(x0i, y1i, k) * (1.0f - ax) + tx(x1i, y1i, k) * ax) * ay;
                    r = c[0]; g = c[1]; b = c[2];
                }
                if (have_fog && !mm_nearest) {
                    const float ux = fx * float(backdrop->fw) - 0.5f, uy = fy * float(backdrop->fh) - 0.5f;
                    const int x0i = std::max(0, std::min(backdrop->fw - 1, int(std::floor(ux)))), y0i = std::max(0, std::min(backdrop->fh - 1, int(std::floor(uy))));
                    const int x1i = std::min(backdrop->fw - 1, x0i + 1), y1i = std::min(backdrop->fh - 1, y0i + 1);
                    const float ax = std::max(0.0f, std::min(1.0f, ux - float(x0i))), ay = std::max(0.0f, std::min(1.0f, uy - float(y0i)));
                    auto ff = [&](int x, int y) { return hud_minimap_fog_factor((*backdrop->fog)[std::size_t(y) * std::size_t(backdrop->fw) + std::size_t(x)]); };
                    const float f = (ff(x0i, y0i) * (1.0f - ax) + ff(x1i, y0i) * ax) * (1.0f - ay) + (ff(x0i, y1i) * (1.0f - ax) + ff(x1i, y1i) * ax) * ay;
                    r *= f; g *= f; b *= f;
                } else if (have_fog) {
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
    render::frame_prof_add("hud.minimap_terrain", std::chrono::duration_cast<std::chrono::duration<double, std::milli>>(std::chrono::steady_clock::now() - t_mm0).count());
    } else {
        fill_rect(img, out_w, out_h, mm.x0, mm.y0, mm.x1, mm.y1, 8, 10, 14, 0.92f);
    }
    if (!(console && console->authored) || OMC_ENV_ONCE("OMC_MINIMAP_BOX"))
        stroke_rect(img, out_w, out_h, mm.x0, mm.y0, mm.x1, mm.y1, 70, 90, 120, 0.9f);
    if (content.minimap_creep && content.creep_alpha > 0.0f && content.creep_w > 0 && content.creep_h > 0 &&
        content.creep_cell > 0.0f && content.minimap_creep->size() >= std::size_t(content.creep_w) * std::size_t(content.creep_h) &&
        mm.w() > 1 && mm.h() > 1 && mext.ok()) {
        const int mw = mm.w(), mh = mm.h();
        std::vector<int> col(std::size_t(mw), -1), row(std::size_t(mh), -1);
        std::vector<int> fcol(std::size_t(mw), 0), frow(std::size_t(mh), 0);
        for (int px = 0; px < mw; ++px) {
            float wx, wy;
            hud_minimap_pixel_to_world(mm, mext, float(mm.x0 + px), float(mm.y0), wx, wy);
            const int cx = int(std::floor((wx - content.creep_x0) / content.creep_cell));
            col[std::size_t(px)] = cx >= 0 && cx < content.creep_w ? cx : -1;
            if (have_fog && map_w > 0.0f) fcol[std::size_t(px)] = std::max(0, std::min(backdrop->fw - 1, int(wx / map_w * float(backdrop->fw))));
        }
        for (int py = 0; py < mh; ++py) {
            float wx, wy;
            hud_minimap_pixel_to_world(mm, mext, float(mm.x0), float(mm.y0 + py), wx, wy);
            const int cy = int(std::floor((wy - content.creep_y0) / content.creep_cell));
            row[std::size_t(py)] = cy >= 0 && cy < content.creep_h ? cy : -1;
            if (have_fog && map_h > 0.0f) frow[std::size_t(py)] = std::max(0, std::min(backdrop->fh - 1, int(wy / map_h * float(backdrop->fh))));
        }
        const std::vector<std::uint8_t>& cr = *content.minimap_creep;
        for (int py = 0; py < mh; ++py) {
            const int cy = row[std::size_t(py)];
            if (cy < 0) continue;
            for (int px = 0; px < mw; ++px) {
                const int cx = col[std::size_t(px)];
                if (cx < 0 || !cr[std::size_t(cy) * std::size_t(content.creep_w) + std::size_t(cx)]) continue;
                float f = 1.0f;
                if (have_fog)
                    f = hud_minimap_fog_factor((*backdrop->fog)[std::size_t(frow[std::size_t(py)]) * std::size_t(backdrop->fw) + std::size_t(fcol[std::size_t(px)])]);
                blend(img, out_w, out_h, mm.x0 + px, mm.y0 + py, std::uint8_t(float(content.creep_rgb[0]) * f + 0.5f),
                      std::uint8_t(float(content.creep_rgb[1]) * f + 0.5f), std::uint8_t(float(content.creep_rgb[2]) * f + 0.5f), content.creep_alpha);
            }
        }
    }
    if (map_w > 0.0f && map_h > 0.0f && mm.w() > 1 && mm.h() > 1) {
        const auto t_bl0 = std::chrono::steady_clock::now();
        std::vector<std::uint32_t> blip_order;
        blip_order.reserve(world.units().size());
        for (const sim::Unit& u : world.units()) blip_order.push_back(u.id);
        if (!content.minimap_priority.empty()) {
            std::stable_sort(blip_order.begin(), blip_order.end(),
                             [&content](std::uint32_t a, std::uint32_t b) {
                                 const std::size_t na = content.minimap_priority.size();
                                 const std::uint8_t ra = a < na ? content.minimap_priority[a] : 0;
                                 const std::uint8_t rb = b < na ? content.minimap_priority[b] : 0;
                                 return ra < rb;
                             });
        }
        struct BlipMark { int bx, by, lo, hi, border; std::uint8_t r, g, b; };
        std::vector<BlipMark> marks;
        auto blip_of = [&](const sim::Unit& u) {
            if (u.minimap_radius.raw() <= 0) return;
            bool has_icon = false;
            for (const HudContent::MinimapIcon& mi : content.minimap_icons)
                if (mi.unit == u.id && mi.rgba) { has_icon = true; break; }
            if (has_icon) return;
            int bx, by;
            hud_minimap_world_to_pixel(mm, mext, float(u.x.to_double()), float(u.y.to_double()), bx, by);
            std::uint8_t r, g, b;
            bool flashing = false;
            for (const HudContent::MinimapFlash& fl : content.minimap_flashes)
                if (fl.unit == u.id) { r = fl.r; g = fl.g; b = fl.b; flashing = true; break; }
            if (flashing) {   }
            else if (u.player >= 0 && std::size_t(u.player) < content.player_colors.size() &&
                     content.player_colors[std::size_t(u.player)].set) {
                const HudContent::MinimapTeamColor& tc = content.player_colors[std::size_t(u.player)];
                const bool sel = std::find(content.minimap_selected.begin(),
                                           content.minimap_selected.end(),
                                           u.id) != content.minimap_selected.end();
                const std::uint8_t* c = sel ? tc.sel_rgb : tc.rgb;
                r = c[0]; g = c[1]; b = c[2];
            }
            else if (u.player == 0) { r = 230; g = 40; b = 40; }
            else if (u.player == 1) { r = 60; g = 90; b = 230; }
            else if (u.resource_left > 0) {
                r = content.resource_rgb[0]; g = content.resource_rgb[1]; b = content.resource_rgb[2];
            }
            else { r = 150; g = 150; b = 150; }
            int ex, ey;
            hud_minimap_world_to_pixel(mm, mext, float((u.x + u.minimap_radius).to_double()),
                                       float(u.y.to_double()), ex, ey);
            const int span = ex - bx;
            const int min_dot = std::max(1, int(content.min_dot_pt * float(out_h) /
                                                    app::kRefHeight + 0.5f));
            const bool floored = 2 * span + 1 < min_dot;
            const int dot_lo = -(min_dot / 2), dot_hi = dot_lo + min_dot - 1;
            const int border = content.border_px > 0 ? content.border_px : 0;
            static const bool blip_odd = std::getenv("OMC_MINIMAP_BLIP_ODD") != nullptr;
            int lo, hi;
            if (floored) {
                lo = dot_lo;
                hi = dot_hi;
            } else if (blip_odd) {
                lo = -span;
                hi = span;
            } else {
                int dx2, dy2;
                hud_minimap_world_to_pixel(mm, mext, float((u.x + u.minimap_radius + u.minimap_radius).to_double()), float(u.y.to_double()), dx2, dy2);
                const int side = std::max(min_dot, dx2 - bx);
                lo = -(side / 2);
                hi = lo + side - 1;
            }
            if (blip_odd) {
                if (border > 0)
                    for (int dy = lo - border; dy <= hi + border; ++dy)
                        for (int dx = lo - border; dx <= hi + border; ++dx) {
                            const int px = bx + dx, py = by + dy;
                            if (px >= mm.x0 && px < mm.x1 && py >= mm.y0 && py < mm.y1)
                                blend(img, out_w, out_h, px, py, content.border_rgb[0], content.border_rgb[1], content.border_rgb[2], 1.0f);
                        }
                for (int dy = lo; dy <= hi; ++dy)
                    for (int dx = lo; dx <= hi; ++dx) {
                        const int px = bx + dx, py = by + dy;
                        if (px >= mm.x0 && px < mm.x1 && py >= mm.y0 && py < mm.y1) blend(img, out_w, out_h, px, py, r, g, b, 1.0f);
                    }
            } else {
                marks.push_back(BlipMark{bx, by, lo, hi, border, r, g, b});
            }
        };
        for (std::uint32_t blip_id : blip_order) {
            if (blip_id >= world.units().size()) continue;
            const sim::Unit& u = world.units()[blip_id];
            if (!u.alive) continue;
            if (blip_id < content.minimap_hidden.size() && content.minimap_hidden[blip_id]) continue;
            if (u.in_gas_structure) continue;
            if (u.loaded && world.cargo_hidden()) continue;
            float fx = float(u.x.to_double()) / map_w;
            float fy = float(u.y.to_double()) / map_h;
            if (fx < 0.0f) fx = 0.0f;
            else if (fx > 1.0f) fx = 1.0f;
            if (fy < 0.0f) fy = 0.0f;
            else if (fy > 1.0f) fy = 1.0f;
            const bool known_resource = u.player < 0 && u.resource_left > 0;
            if (have_fog && backdrop->viewer_player >= 0 && !known_resource &&
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
            blip_of(u);
        }
        for (const HudContent::MinimapSnapshot& sn : content.minimap_snapshots) {
            sim::Unit v;
            v.id = 0xFFFFFFFFu;
            v.player = sn.player;
            v.x = sim::Fixed::from_double(sn.x);
            v.y = sim::Fixed::from_double(sn.y);
            v.minimap_radius = sim::Fixed::from_double(sn.radius);
            blip_of(v);
        }
        for (int pass = 0; pass < 2; ++pass)
            for (const BlipMark& mk : marks) {
                const int grow = pass == 0 ? mk.border : 0;
                if (pass == 0 && mk.border <= 0) continue;
                for (int dy = mk.lo - grow; dy <= mk.hi + grow; ++dy)
                    for (int dx = mk.lo - grow; dx <= mk.hi + grow; ++dx) {
                        const int px = mk.bx + dx, py = mk.by + dy;
                        if (px < mm.x0 || px >= mm.x1 || py < mm.y0 || py >= mm.y1) continue;
                        if (pass == 0) blend(img, out_w, out_h, px, py, content.border_rgb[0], content.border_rgb[1], content.border_rgb[2], 1.0f);
                        else blend(img, out_w, out_h, px, py, mk.r, mk.g, mk.b, 1.0f);
                    }
            }
        render::frame_prof_add("hud.blips", std::chrono::duration_cast<std::chrono::duration<double, std::milli>>(std::chrono::steady_clock::now() - t_bl0).count());

        for (const HudContent::MinimapIcon& mi : content.minimap_icons) {
            if (!mi.rgba || mi.tw <= 0 || mi.th <= 0) continue;
            if (mi.rgba->size() < std::size_t(mi.tw) * std::size_t(mi.th) * 4) continue;
            int cx, cy, ex, ey;
            hud_minimap_world_to_pixel(mm, mext, mi.x, mi.y, cx, cy);
            hud_minimap_world_to_pixel(mm, mext, mi.x + 1.0f, mi.y, ex, ey);
            const float px_per_world = float(ex - cx);
            int half = int(px_per_world * mi.scale + 0.5f);
            if (half < 2) half = 2;
            if (half > 64) half = 64;
            for (int dy = -half; dy <= half; ++dy) {
                const int py = cy + dy;
                if (py < mm.y0 || py >= mm.y1) continue;
                const int sy = (dy + half) * mi.th / (2 * half + 1);
                for (int dx = -half; dx <= half; ++dx) {
                    const int px = cx + dx;
                    if (px < mm.x0 || px >= mm.x1) continue;
                    const int sx = (dx + half) * mi.tw / (2 * half + 1);
                    const std::size_t si = (std::size_t(sy) * std::size_t(mi.tw) + std::size_t(sx)) * 4;
                    const std::uint8_t a = (*mi.rgba)[si + 3];
                    if (a == 0) continue;
                    blend(img, out_w, out_h, px, py, (*mi.rgba)[si], (*mi.rgba)[si + 1],
                          (*mi.rgba)[si + 2], float(a) / 255.0f);
                }
            }
        }

        for (const HudContent::MinimapRange& rg : content.minimap_ranges) {
            int cx, cy, ex, ey;
            hud_minimap_world_to_pixel(mm, mext, rg.x, rg.y, cx, cy);
            hud_minimap_world_to_pixel(mm, mext, rg.x + rg.radius, rg.y, ex, ey);
            const float radius = float(std::abs(ex - cx));
            if (radius < 1.0f) continue;
            const int span = int(radius) + 2;
            for (int dy = -span; dy <= span; ++dy)
                for (int dx = -span; dx <= span; ++dx) {
                    const float cover = 1.0f - std::fabs(std::sqrt(float(dx * dx + dy * dy)) - radius);
                    if (cover <= 0.0f) continue;
                    const int px = cx + dx, py = cy + dy;
                    if (px >= mm.x0 && px < mm.x1 && py >= mm.y0 && py < mm.y1)
                        blend(img, out_w, out_h, px, py, content.range_rgb[0], content.range_rgb[1], content.range_rgb[2],
                              content.range_alpha * cover);
                }
        }

        for (const std::pair<float, float>& b : content.radar_blips) {
            int bx, by;
            hud_minimap_world_to_pixel(mm, mext, b.first, b.second, bx, by);
            for (int dy = 0; dy < 2; ++dy)
                for (int dx = 0; dx < 2; ++dx) {
                    const int px = bx + dx, py = by + dy;
                    if (px >= mm.x0 && px < mm.x1 && py >= mm.y0 && py < mm.y1)
                        blend(img, out_w, out_h, px, py, content.blip_rgb[0], content.blip_rgb[1],
                              content.blip_rgb[2], content.blip_alpha);
                }
        }

        for (const HudContent::MinimapPing& p : content.pings) {
            if (p.ping_time <= 0.0f || p.age >= p.ping_time) continue;
            int cx, cy;
            hud_minimap_world_to_pixel(mm, mext, p.x, p.y, cx, cy);
            const float pulse = std::fmod(p.age * 3.0f / p.ping_time, 1.0f);
            const float radius = 2.0f + 5.0f * pulse;
            const float alpha = (1.0f - pulse) * (1.0f - p.age / p.ping_time);
            const int span = int(radius) + 1;
            for (int dy = -span; dy <= span; ++dy)
                for (int dx = -span; dx <= span; ++dx) {
                    const float d = std::sqrt(float(dx * dx + dy * dy));
                    if (std::fabs(d - radius) > 0.75f) continue;
                    const int px = cx + dx, py = cy + dy;
                    if (px >= mm.x0 && px < mm.x1 && py >= mm.y0 && py < mm.y1)
                        blend(img, out_w, out_h, px, py, p.r, p.g, p.b, alpha);
                }
        }

        if (cam && cam->valid) {
            int px[4], py[4];
            for (int k = 0; k < 4; ++k) {
                hud_minimap_world_to_pixel(mm, mext, cam->x[k], cam->y[k], px[k], py[k]);
            }
            for (int k = 0; k < 4; ++k)
                draw_line(img, out_w, out_h, px[k], py[k], px[(k + 1) % 4], py[(k + 1) % 4],
                          mm.x0, mm.y0, mm.x1, mm.y1, content.frustum_rgb[0], content.frustum_rgb[1],
                          content.frustum_rgb[2], 1.0f);
        }
    }

    const int equip_n = int(content.equipment.size());
    if (equip_n > 0 && hud_equipment_cell(out_w, out_h, 0, console, equip_n).w() > 0) {
        for (int q = 0; q < equip_n; ++q) {
            const HudRect ec = hud_equipment_cell(out_w, out_h, q, console, equip_n);
            if (ec.w() <= 0) break;
            fill_rect(img, out_w, out_h, ec.x0, ec.y0, ec.x1, ec.y1, 10, 12, 16, 0.85f);
            if (std::getenv("OMC_NO_EQUIPMENT_BORDER")) stroke_rect(img, out_w, out_h, ec.x0, ec.y0, ec.x1, ec.y1, 60, 70, 80, 0.9f);
        }
    } else if (!content.equipment.empty() || content.lead_kills > 0) {
        HudRect ep;
        if (console && console->authored && authored_rect(console, console->authored->equipment, out_h, ep) &&
            ep.w() > 8 && ep.h() > 8) {
            const int cell = std::min(ep.h(), 64 * int(out_h) / int(kRefHeight) > 8 ? 64 * int(out_h) / int(kRefHeight) : ep.h());
            int x = ep.x0;
            int drawn = 0;
            for (const EquipmentEntry& e : content.equipment) {
                const HudRect ec = hud_equipment_cell(out_w, out_h, drawn, console);
                const int cx0 = ec.w() > 0 ? ec.x0 : x, cy0 = ec.w() > 0 ? ec.y0 : ep.y0;
                const int cx1 = ec.w() > 0 ? ec.x1 : x + cell, cy1 = ec.w() > 0 ? ec.y1 : ep.y0 + cell;
                if (cx1 > ep.x1) break;
                fill_rect(img, out_w, out_h, cx0, cy0, cx1, cy1, 18, 24, 32, 0.80f);
                stroke_rect(img, out_w, out_h, cx0, cy0, cx1, cy1, 120, 150, 190, 0.95f);
                const std::string lvl = std::to_string(e.level);
                const int lw = draw::text_w(lvl, 1);
                draw_text(img, out_w, out_h, cx1 - 8 - lw, cy1 - 3 - ui::kFontH, lvl, 1, 235, 236, 210,
                          cx0, cy0, cx1, cy1);
                x = cx1 + 2;
                ++drawn;
            }
            if (content.lead_kills > 0)
                draw_text(img, out_w, out_h, x + 4, ep.y0 + 4, std::to_string(content.lead_kills), 2, 235, 236,
                          210, x, ep.y0, ep.x1, ep.y0 + cell);
        }
    }

    if (content.idle_workers > 0 && !content.widget_art) {
        const HudRect ib = hud_idle_button_rect(out_w, out_h, console);
        if (ib.w() > 2 && ib.h() > 2) {
            fill_rect(img, out_w, out_h, ib.x0, ib.y0, ib.x1, ib.y1, 18, 24, 32, 0.80f);
            stroke_rect(img, out_w, out_h, ib.x0, ib.y0, ib.x1, ib.y1, 120, 150, 190, 0.95f);
            const std::string n = std::to_string(content.idle_workers);
            draw_text(img, out_w, out_h, ib.x0 + 6, ib.y0 + ib.h() / 2 - ui::kFontH, n, 2, 235, 236, 210,
                      ib.x0, ib.y0, ib.x1, ib.y1);
        }
    }

    if (content.army_units > 0 && !content.widget_art) {
        const HudRect ab = hud_army_button_rect(out_w, out_h, console);
        if (ab.w() > 2 && ab.h() > 2) {
            fill_rect(img, out_w, out_h, ab.x0, ab.y0, ab.x1, ab.y1, 18, 24, 32, 0.80f);
            stroke_rect(img, out_w, out_h, ab.x0, ab.y0, ab.x1, ab.y1, 120, 150, 190, 0.95f);
            const std::string n = std::to_string(content.army_units);
            draw_text(img, out_w, out_h, ab.x0 + 6, ab.y0 + ab.h() / 2 - ui::kFontH, n, 2, 235, 236, 210,
                      ab.x0, ab.y0, ab.x1, ab.y1);
        }
    }

    static const bool kWarpButtonArtOld = std::getenv("OMC_WARP_BUTTON_ART_OLD") != nullptr;
    if (content.warp_gates_ready > 0 && (!content.widget_art || kWarpButtonArtOld)) {
        const HudRect wb = hud_warp_button_rect(out_w, out_h, console);
        if (wb.w() > 2 && wb.h() > 2) {
            fill_rect(img, out_w, out_h, wb.x0, wb.y0, wb.x1, wb.y1, 18, 24, 32, 0.80f);
            stroke_rect(img, out_w, out_h, wb.x0, wb.y0, wb.x1, wb.y1, 120, 150, 190, 0.95f);
            const std::string n = std::to_string(content.warp_gates_ready);
            draw_text(img, out_w, out_h, wb.x0 + 6, wb.y0 + wb.h() / 2 - ui::kFontH, n, 2, 235, 236, 210,
                      wb.x0, wb.y0, wb.x1, wb.y1);
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
        HudRect err_rect{0, 0, 0, 0}, warn_rect{0, 0, 0, 0};
        if (console && console->authored) {
            authored_rect(console, console->authored->error_display, out_h, err_rect);
            authored_rect(console, console->authored->warning_display, out_h, warn_rect);
        }
        const bool authored_msgs = err_rect.w() > 8 && warn_rect.w() > 8;
        int err_y = panel_y0 - ui::kFontH * 2 - 8;
        int ul_y = kPad + 2;
        int a_err_y = err_rect.y0 + 2, a_warn_y = warn_rect.y0 + 2;
        const int adv = (ui::kFontW + 1) * 2;
        const int err_cap = (console && console->authored && console->authored->error_timing.max_messages > 0)
                                ? console->authored->error_timing.max_messages : 0;
        const int warn_cap = (console && console->authored && console->authored->warning_timing.max_messages > 0)
                                 ? console->authored->warning_timing.max_messages : 0;
        const float err_fade_s = (console && console->authored)
                                     ? console->authored->error_timing.fade_ms / 1000.0f : 0.0f;
        const float warn_fade_s = (console && console->authored)
                                      ? console->authored->warning_timing.fade_ms / 1000.0f : 0.0f;
        int err_drawn = 0, warn_drawn = 0;
        auto msg_px = [out_h](const HudMessageStyle& st) {
            if (!st.set || st.height_pt <= 0) return 0;
            const int px = int(float(st.height_pt) * float(out_h) / app::kRefHeight + 0.5f);
            return px > 0 ? px : 0;
        };
        const int err_px_h = msg_px(content.error_style);
        const int warn_px_h = msg_px(content.warning_style);
        auto fade_of = [](const HudMessageLine& m, float fade_s) {
            if (fade_s <= 0.0f || m.ttl0 <= 0.0f || m.ttl <= 0.0f) return 1.0f;
            if (m.ttl >= fade_s) return 1.0f;
            const float f = m.ttl / fade_s;
            return f < 0.0f ? 0.0f : f;
        };
        auto dim = [](std::uint8_t c, float f) { return std::uint8_t(float(c) * f + 0.5f); };
        static const bool kMsgOld = std::getenv("OMC_HUD_MESSAGE_STYLE_OLD") != nullptr;
        const bool saved_shadow_off = draw::text_shadow_off();
        const int saved_shadow_px = draw::text_shadow_px();
        auto use_shadow = [&](const HudMessageStyle& st) {
            if (kMsgOld || !st.set) {
                draw::text_shadow_off() = saved_shadow_off;
                draw::text_shadow_px() = saved_shadow_px;
                return;
            }
            draw::text_shadow_off() = !st.shadow;
            const int sp = int(float(st.shadow_offset) * float(out_h) / app::kRefHeight + 0.5f);
            draw::text_shadow_px() = sp > 1 ? sp : 1;
        };
        const bool err_styled = !kMsgOld && content.error_style.set;
        for (const HudMessageLine& m : content.messages) {
            const int tw = int(m.text.size()) * adv;
            if (authored_msgs && m.zone == MsgZone::Error) {
                if (err_cap > 0 && err_drawn >= err_cap) continue;
                if (a_err_y + ui::kFontH * 2 > err_rect.y1) continue;
                const float f = fade_of(m, err_fade_s);
                const std::uint8_t er = content.error_style.set ? content.error_style.r : 255;
                const std::uint8_t eg = content.error_style.set ? content.error_style.g : 130;
                const std::uint8_t eb = content.error_style.set ? content.error_style.b : 130;
                draw::set_forced_font_px(err_px_h);
                const int etw = draw::text_w(m.text, 2);
                int ex = err_rect.x0;
                if (content.error_style.set && content.error_style.centred) {
                    ex = err_rect.x0 + (err_rect.w() - etw) / 2;
                    if (ex < err_rect.x0) ex = err_rect.x0;
                }
                if (!err_styled)
                    fill_rect(img, out_w, out_h, ex - 4, a_err_y - 2,
                              ex + etw + 4, a_err_y + ui::kFontH * 2 + 2, 60, 12, 12, 0.65f * f);
                use_shadow(content.error_style);
                draw_text(img, out_w, out_h, ex, a_err_y, m.text, 2, dim(er, f), dim(eg, f),
                          dim(eb, f), 0, 0, W, H);
                use_shadow(HudMessageStyle{});
                draw::set_forced_font_px(0);
                a_err_y += ui::kFontH * 2 + 6;
                ++err_drawn;
            } else if (authored_msgs && m.zone != MsgZone::Error) {
                if (warn_cap > 0 && warn_drawn >= warn_cap) continue;
                std::uint8_t r = 235, g = 225, b = 130;
                if (m.zone == MsgZone::Transmission) { r = 150; g = 210; b = 240; }
                const HudMessageStyle& zst = (!kMsgOld && m.zone == MsgZone::Transmission) ? content.transmission_style : content.warning_style;
                const bool zstyled = zst.set && (m.zone == MsgZone::Warning || (!kMsgOld && m.zone == MsgZone::Transmission));
                if (zstyled) {
                    r = zst.r; g = zst.g; b = zst.b;
                }
                const float f = fade_of(m, warn_fade_s);
                const bool centre = content.warning_style.set ? content.warning_style.centred : true;
                draw::set_forced_font_px(warn_px_h);
                const int wtw = draw::text_w(m.text, 2);
                int x = centre ? warn_rect.x0 + (warn_rect.w() - wtw) / 2 : warn_rect.x0;
                if (x < warn_rect.x0) x = warn_rect.x0;
                if (a_warn_y + ui::kFontH * 2 <= warn_rect.y1) {
                    use_shadow(zstyled ? zst : HudMessageStyle{});
                    draw_text(img, out_w, out_h, x, a_warn_y, m.text, 2, dim(r, f), dim(g, f),
                              dim(b, f), 0, 0, W, H);
                    use_shadow(HudMessageStyle{});
                    ++warn_drawn;
                }
                draw::set_forced_font_px(0);
                a_warn_y += ui::kFontH * 2 + 4;
            } else if (m.zone == MsgZone::Error) {
                int x = (W - tw) / 2;
                if (x < kPad) x = kPad;
                if (!err_styled)
                    fill_rect(img, out_w, out_h, x - 4, err_y - 2, x + tw + 4,
                              err_y + ui::kFontH * 2 + 2, 60, 12, 12, 0.65f);
                use_shadow(content.error_style);
                draw_text(img, out_w, out_h, x, err_y, m.text, 2, err_styled ? content.error_style.r : 255,
                          err_styled ? content.error_style.g : 130, err_styled ? content.error_style.b : 130, 0, 0, W, H);
                use_shadow(HudMessageStyle{});
                err_y -= ui::kFontH * 2 + 6;
            } else {
                std::uint8_t r = 235, g = 225, b = 130;
                if (m.zone == MsgZone::Transmission) {
                    r = 150;
                    g = 210;
                    b = 240;
                }
                const HudMessageStyle& zst = m.zone == MsgZone::Transmission ? content.transmission_style : content.warning_style;
                const bool zstyled = !kMsgOld && zst.set && (m.zone == MsgZone::Warning || m.zone == MsgZone::Transmission);
                if (zstyled) { r = zst.r; g = zst.g; b = zst.b; }
                use_shadow(zstyled ? zst : HudMessageStyle{});
                draw_text(img, out_w, out_h, kPad, ul_y, m.text, 2, r, g, b, 0, 0, W, H);
                use_shadow(HudMessageStyle{});
                ul_y += ui::kFontH * 2 + 4;
            }
        }
    }
}

}
