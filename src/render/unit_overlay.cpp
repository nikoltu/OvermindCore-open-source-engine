#include "omc/render/unit_overlay.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace omc::render {

namespace {

int g_dirty_y0 = 0;
int g_dirty_y1 = -1;

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
    if (y < g_dirty_y0) g_dirty_y0 = y;
    if (y > g_dirty_y1) g_dirty_y1 = y;
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

void draw_status_row(std::vector<uint8_t>& buf, uint32_t w, uint32_t h, int left, int top, double frac,
                     const Color& c, int segments, double trail = -1.0, int width = UnitOverlay::kStatusBarWidth,
                     const Color& kEmpty = Color{80, 80, 80, 255},
                     const Color& kBackground = Color{0, 0, 0, 255},
                     int height = UnitOverlay::kStatusBarHeight) {
    const int W = width, H = height;
    if (frac < 0.0) frac = 0.0;
    if (frac > 1.0) frac = 1.0;
    fill_rect(buf, w, h, left, top, left + W, top + H, kBackground);
    const int inner_w = W - 2, inner_h = H - 2;
    const int fill_w = int(std::lround(frac * double(inner_w)));
    fill_rect(buf, w, h, left + 1, top + 1, left + 1 + fill_w, top + 1 + inner_h, c);
    fill_rect(buf, w, h, left + 1 + fill_w, top + 1, left + 1 + inner_w, top + 1 + inner_h, kEmpty);
    if (trail > frac) {
        constexpr Color kReduction{255, 0, 0, 255};
        if (trail > 1.0) trail = 1.0;
        const int trail_w = int(std::lround(trail * double(inner_w)));
        fill_rect(buf, w, h, left + 1 + fill_w, top + 1, left + 1 + trail_w, top + 1 + inner_h, kReduction);
    }
    for (int s = 1; s < segments; ++s) {
        const int x = left + 1 + int(std::lround(double(inner_w) * double(s) / double(segments)));
        fill_rect(buf, w, h, x, top + 1, x + 1, top + 1 + inner_h, kBackground);
    }
}

void draw_shaded_row(std::vector<uint8_t>& buf, uint32_t w, uint32_t h, int left, int top, int width, double frac,
                     const Color& fill, const Color& empty, int segments, double trail, const Color& reduction) {
    constexpr Color kFrame{17, 18, 27, 255};
    static const double kShade[5] = {0.82, 1.04, 1.0, 1.0, 0.66};
    const int H = UnitOverlay::kStatusBarHeight;
    if (frac < 0.0) frac = 0.0;
    if (frac > 1.0) frac = 1.0;
    fill_rect(buf, w, h, left, top, left + width, top + H, kFrame);
    const int inner_w = width - 4, ix = left + 2;
    const int fill_w = int(std::lround(frac * double(inner_w)));
    const int trail_w = trail > frac ? int(std::lround((trail > 1.0 ? 1.0 : trail) * double(inner_w))) : fill_w;
    static const bool kPartialCellOld = std::getenv("OMC_STATUS_PARTIAL_CELL_OLD") != nullptr;
    const bool cell_mix = !kPartialCellOld && segments > 1;
    auto shade = [](const Color& c, double k) {
        auto ch = [&](uint8_t v) { const double x = double(v) * k; return uint8_t(x > 255.0 ? 255.0 : x); };
        return Color{ch(c.r), ch(c.g), ch(c.b), 255};
    };
    for (int r = 0; r < H - 2; ++r) {
        const int y = top + 1 + r;
        fill_rect(buf, w, h, ix, y, ix + fill_w, y + 1, shade(fill, kShade[r]));
        fill_rect(buf, w, h, ix + fill_w, y, ix + trail_w, y + 1, shade(reduction, kShade[r]));
        fill_rect(buf, w, h, ix + trail_w, y, ix + inner_w, y + 1, shade(empty, kShade[r]));
        if (cell_mix) {
            const double covered = frac * double(segments);
            const int cell = std::min(segments, int(std::floor(covered)));
            const int x0 = ix + int(std::lround(double(inner_w) * double(cell) / double(segments)));
            fill_rect(buf, w, h, ix, y, x0, y + 1, shade(fill, kShade[r]));
            fill_rect(buf, w, h, x0, y, ix + inner_w, y + 1, shade(empty, kShade[r]));
            const double k = covered - double(cell);
            if (cell < segments && k > 0.0) {
                const int x1 = ix + int(std::lround(double(inner_w) * double(cell + 1) / double(segments)));
                auto mix = [&](uint8_t a, uint8_t b) { return uint8_t(std::lround(double(a) + (double(b) - double(a)) * k)); };
                const Color m{mix(empty.r, fill.r), mix(empty.g, fill.g), mix(empty.b, fill.b), 255};
                fill_rect(buf, w, h, x0, y, x1, y + 1, shade(m, kShade[r]));
            }
        }
    }
    for (int s = 1; s < segments; ++s) {
        const int x = ix + int(std::lround(double(inner_w) * double(s) / double(segments)));
        fill_rect(buf, w, h, x - 1, top + 1, x + 1, top + H - 1, kFrame);
    }
}

}

std::vector<uint8_t> UnitOverlay::render_status(const sim::World& w, const Mat4& mvp, uint32_t view_w,
                                                uint32_t view_h, const StatusBarOptions& o) const {
    std::vector<uint8_t> rgba;
    int y0 = 0, y1 = -1;
    render_status_into(rgba, y0, y1, w, mvp, view_w, view_h, o);
    return rgba;
}

void UnitOverlay::render_status_into(std::vector<uint8_t>& rgba, int& dirty_y0, int& dirty_y1, const sim::World& w,
                                     const Mat4& mvp, uint32_t view_w, uint32_t view_h, const StatusBarOptions& o) const {
    const size_t bytes = size_t(view_w) * size_t(view_h) * 4;
    if (rgba.size() != bytes) {
        rgba.assign(bytes, 0);
    } else if (dirty_y0 <= dirty_y1) {
        const int y0 = dirty_y0 < 0 ? 0 : dirty_y0;
        const int y1 = dirty_y1 >= int(view_h) ? int(view_h) - 1 : dirty_y1;
        if (y0 <= y1) std::memset(rgba.data() + size_t(y0) * size_t(view_w) * 4, 0, size_t(y1 - y0 + 1) * size_t(view_w) * 4);
    }
    dirty_y0 = 0;
    dirty_y1 = -1;
    if (view_w == 0 || view_h == 0) return;
    g_dirty_y0 = int(view_h);
    g_dirty_y1 = -1;
    using Mode = StatusBarOptions::Mode;

    constexpr Color kLifeBand[4] = {{208, 34, 0, 255}, {229, 129, 0, 255}, {229, 221, 0, 255}, {22, 229, 0, 255}};
    static const bool kColoursOld = std::getenv("OMC_STATUS_COLOURS_OLD") != nullptr;
    static const bool kTrailOld = std::getenv("OMC_STATUS_TRAIL_OLD") != nullptr;
    static const bool kBandsOld = std::getenv("OMC_STATUS_LEGACY_BANDS_OLD") != nullptr;
    const Color kShield = kColoursOld ? Color{0, 0, 255, 255} : Color{54, 56, 190, 255};
    const Color kEnergy = kColoursOld ? Color{150, 40, 220, 255} : Color{136, 79, 195, 255};
    const Color kProgress = kColoursOld ? Color{0, 200, 200, 255} : Color{45, 141, 165, 255};
    const Color kDuration = kColoursOld ? Color{120, 120, 120, 255} : Color{114, 116, 136, 255};
    const Color kDurationEmpty = kColoursOld ? Color{40, 40, 40, 255} : Color{37, 39, 52, 255};
    const Color kSeat = kColoursOld ? Color{255, 255, 255, 255} : Color{223, 223, 224, 255};
    constexpr Color kTeamOwn{0, 255, 0, 255};
    constexpr Color kTeamAlly{11, 105, 1, 255};
    constexpr Color kTeamEnemy{150, 15, 50, 255};

    auto selected = [&](uint32_t id) {
        return o.selection && std::find(o.selection->begin(), o.selection->end(), id) != o.selection->end();
    };
    static const bool kResearchRowOld = std::getenv("OMC_RESEARCH_PANE_OLD") != nullptr;
    auto queue_progress = [&](uint32_t id, sim::Fixed& f) {
        if (w.pending_progress(id, f)) return true;
        if (kResearchRowOld) return false;
        const std::vector<sim::World::ResearchSlotView> r = w.research_at(id);
        if (r.empty()) return false;
        f = sim::Fixed::from_double(double(r.front().progress));
        return true;
    };
    auto segments_of = [](const sim::Fixed& max) {
        const double m = max.to_double();
        int n = int(std::ceil(m / double(kStatusSegmentValue)));
        return n < 1 ? 1 : n;
    };

    for (const sim::Unit& u : w.units()) {
        if (!u.alive || u.player < 0 || u.loaded || u.in_gas_structure || u.hp_max.raw() <= 0) continue;
        const float bar_off = (o.bar_offset && u.id < o.bar_offset->size()) ? (*o.bar_offset)[u.id] : 0.0f;
        if (bar_off > 0.0f) {
            const bool own = o.own_rows && u.player == o.viewer_player && !u.constructing;
            sim::Fixed tpf;
            const bool training = own && queue_progress(u.id, tpf);
            const bool cargo = own && u.cargo_space > 0 && (!o.cargo_ok || u.id >= o.cargo_ok->size() || (*o.cargo_ok)[u.id]);
            const bool shield = u.shields_max.raw() > 0, energy = u.energy_max.raw() > 0;
            const bool hurt = u.hp < u.hp_max || (shield && u.shields < u.shields_max) || (energy && u.energy < u.energy_max);
            bool vitals = o.reveal_all;
            switch (o.mode) {
                case Mode::Always: vitals = true; break;
                case Mode::Damaged: vitals = vitals || hurt; break;
                case Mode::Selected: vitals = vitals || selected(u.id); break;
                case Mode::Normal:
                case Mode::Targeting: vitals = vitals || (selected(u.id) && (hurt || !o.own_rows)); break;
            }
            static const bool kSalvageBarOld = std::getenv("OMC_SALVAGE_BAR_OLD") != nullptr;
            const bool salvaging = !kSalvageBarOld && u.salvage_timer.raw() > 0 && u.salvage_total.raw() > 0 &&
                                   o.viewer_player >= 0 && (u.player == o.viewer_player || w.are_allied(u.player, o.viewer_player));
            if (!vitals && !cargo && !training && !salvaging) continue;
            const float ox = float(u.x.to_double()), oy = float(u.y.to_double());
            const float oz = (o.anchor_z && u.id < o.anchor_z->size()) ? (*o.anchor_z)[u.id] : 0.0f;
            float spx, spy;
            if (!project(mvp, ox, oy, oz, view_w, view_h, spx, spy)) continue;
            const double ps = double(o.px_scale);
            const float aw = (o.bar_width && u.id < o.bar_width->size()) ? (*o.bar_width)[u.id] : 0.0f;
            const int bw = aw > 0.0f ? int(std::lround(aw)) : std::max(8, int(std::lround(double(kStatusBarWidth) * ps)));
            const int segs = std::max(1, int(std::lround(double(bw) / (10.0 * ps))));
            const int cx = int(std::lround(spx)), cy = int(std::lround(spy));
            if (cx < -bw || cx >= int(view_w) + bw || cy < 0 || cy >= int(view_h) + 400) continue;
            const int left = cx - bw / 2 - 3, row_w = bw + 5;
            const int last_top = cy - int(std::lround(double(bar_off + float(kStatusBarDistance - kStatusBarHeight)) * ps * 0.75)) - kStatusBarHeight;
            auto treated = [](const Color& c) {
                const double l = 0.299 * c.r + 0.587 * c.g + 0.114 * c.b;
                auto ch = [&](double v, double k) { const double x = 0.478 * v + 0.356 * l + k; return uint8_t(x < 0.0 ? 0.0 : (x > 255.0 ? 255.0 : x)); };
                return Color{ch(c.r, 3.0), ch(c.g, 5.0), ch(c.b, 25.0), 255};
            };
            auto screen = [&](const Color& c) { return kColoursOld ? treated(c) : c; };
            constexpr Color kScreenLife[4] = {{141, 63, 71, 255}, {160, 115, 76, 255}, {147, 145, 73, 255}, {64, 165, 76, 255}};
            constexpr Color kScreenEmpty{70, 72, 92, 255};
            struct Row { double frac; Color fill; Color empty; int segs; double trail; };
            Row rows[8];
            int n = 0;
            static const bool kWholeStack = std::getenv("OMC_STATUS_DAMAGED_WHOLE_STACK") != nullptr;
            const bool short_only = !kWholeStack && o.mode == Mode::Damaged && !o.reveal_all && !selected(u.id);
            const bool row_shield = shield && (!short_only || u.shields < u.shields_max);
            const bool row_life = !short_only || u.hp < u.hp_max;
            const bool row_energy = energy && (!short_only || u.energy < u.energy_max);
            if (vitals) {
                if (row_shield) {
                    const double st = (o.shield_trail && u.id < o.shield_trail->size()) ? double((*o.shield_trail)[u.id]) : -1.0;
                    rows[n++] = Row{u.shields.to_double() / u.shields_max.to_double(), screen(kShield), kScreenEmpty, segs, kTrailOld ? st : -1.0};
                }
                const double lf = u.hp.to_double() / u.hp_max.to_double();
                int band = int(std::floor((lf < 0.0 ? 0.0 : (lf > 1.0 ? 1.0 : lf)) * 7.0)) / 2;
                if (band > 3) band = 3;
                Color life = kScreenLife[band];
                if (o.team_colored)
                    life = treated(u.player == o.viewer_player ? kTeamOwn
                                   : (o.viewer_player >= 0 && w.are_allied(u.player, o.viewer_player)) ? kTeamAlly : kTeamEnemy);
                const double lt = (o.life_trail && u.id < o.life_trail->size()) ? double((*o.life_trail)[u.id]) : -1.0;
                if (row_life) rows[n++] = Row{lf, life, kScreenEmpty, segs, kTrailOld ? lt : -1.0};
                if (row_energy) rows[n++] = Row{u.energy.to_double() / u.energy_max.to_double(), screen(kEnergy), kScreenEmpty, segs, -1.0};
                if (u.constructing) rows[n++] = Row{u.build_progress.to_double(), Color{45, 141, 165, 255}, kScreenEmpty, 1, -1.0};
                if (u.time_life.raw() > 0 && u.time_life_max.raw() > 0)
                    rows[n++] = Row{u.time_life.to_double() / u.time_life_max.to_double(), screen(kDuration), screen(kDurationEmpty), 1, -1.0};
            }
            if (cargo)
                rows[n++] = Row{double(w.cargo_used(u.id)) / double(u.cargo_space), screen(kSeat), Color{53, 49, 46, 255},
                                int(u.cargo_space), -1.0};
            for (int i = 0; i < n; ++i)
                draw_shaded_row(rgba, view_w, view_h, left, last_top - 9 * (n - 1 - i), row_w, rows[i].frac, rows[i].fill, rows[i].empty,
                                rows[i].segs, rows[i].trail, treated(Color{255, 0, 0, 255}));
            if (training) {
                const double f = tpf.to_double();
                draw_shaded_row(rgba, view_w, view_h, left, last_top + 18, row_w, f, Color{45, 141, 165, 255}, kScreenEmpty, 1, -1.0,
                                Color{0, 0, 0, 255});
            }
            if (salvaging)
                draw_shaded_row(rgba, view_w, view_h, left, last_top + 8, row_w, u.salvage_timer.to_double() / u.salvage_total.to_double(),
                                screen(kDuration), screen(kDurationEmpty), 1, -1.0, Color{0, 0, 0, 255});
            continue;
        }
        if (o.own_rows && u.player == o.viewer_player && !u.constructing) {
            sim::Fixed pf;
            const bool training = queue_progress(u.id, pf);
            if (u.cargo_space > 0 || training) {
                const float ax = float(u.x.to_double()), ay = float(u.y.to_double());
                const float az = (o.anchor_z && u.id < o.anchor_z->size()) ? (*o.anchor_z)[u.id] : 0.0f;
                float px, py;
                if (project(mvp, ax, ay, az, view_w, view_h, px, py)) {
                    const float authored_w = (o.bar_width && u.id < o.bar_width->size()) ? (*o.bar_width)[u.id] : 0.0f;
                    const int bw = authored_w > 0.0f ? int(std::lround(authored_w))
                                                     : std::max(24, int(std::lround(60.0 * u.radius.to_double())));
                    const int cx = int(std::lround(px)), cy = int(std::lround(py));
                    const int left = cx - bw / 2;
                    const int life_top = cy - kStatusBarDistance - kStatusBarHeight;
                    const int row_h = kStatusBarHeight, pitch = 9;
                    constexpr Color kFrame{16, 17, 25, 255};
                    if (u.cargo_space > 0) {
                        const double cf = double(w.cargo_used(u.id)) / double(u.cargo_space);
                        draw_status_row(rgba, view_w, view_h, left, life_top - pitch, cf, Color{22, 229, 0, 255}, int(u.cargo_space), -1.0, bw,
                                        Color{53, 49, 46, 255}, kFrame, row_h);
                    }
                    if (training) {
                        double f = pf.to_double();
                        f = f < 0.0 ? 0.0 : (f > 1.0 ? 1.0 : f);
                        draw_status_row(rgba, view_w, view_h, left, life_top + pitch, f, Color{45, 141, 165, 255}, 1, -1.0, bw,
                                        Color{70, 72, 92, 255}, kFrame, row_h);
                    }
                }
            }
        }
        const bool has_shield = u.shields_max.raw() > 0;
        const bool has_energy = u.energy_max.raw() > 0;
        const bool has_progress = u.constructing;
        const bool has_duration = u.time_life.raw() > 0 && u.time_life_max.raw() > 0;
        const bool damaged = u.hp < u.hp_max || (has_shield && u.shields < u.shields_max) ||
                             (has_energy && u.energy < u.energy_max);
        bool show = o.reveal_all;
        switch (o.mode) {
            case Mode::Always: show = true; break;
            case Mode::Damaged: show = show || damaged; break;
            case Mode::Selected: show = show || selected(u.id); break;
            case Mode::Normal:
            case Mode::Targeting: show = show || (selected(u.id) && (damaged || !o.own_rows)); break;
        }
        if (!show) continue;

        const float ax = float(u.x.to_double()), ay = float(u.y.to_double());
        const float az = (o.anchor_z && u.id < o.anchor_z->size()) ? (*o.anchor_z)[u.id] : 0.0f;
        float px, py;
        if (!project(mvp, ax, ay, az, view_w, view_h, px, py)) continue;
        const int cx = int(std::lround(px)), cy = int(std::lround(py));
        if (cx < -kStatusBarWidth || cy < -kStatusBarDistance - 3 * kStatusBarHeight ||
            cx >= int(view_w) + kStatusBarWidth || cy >= int(view_h) + kStatusBarHeight)
            continue;
        const int left = cx - kStatusBarWidth / 2;
        int top = cy - kStatusBarDistance - kStatusBarHeight;
        const int life_top = has_shield ? top + (kStatusBarHeight - 1) : top;
        if (has_shield) {
            const double sf = u.shields.to_double() / u.shields_max.to_double();
            const double st = (o.shield_trail && u.id < o.shield_trail->size()) ? double((*o.shield_trail)[u.id]) : -1.0;
            draw_status_row(rgba, view_w, view_h, left, top, sf, kShield, segments_of(u.shields_max), kTrailOld ? st : -1.0);
        }
        const double lf = u.hp.to_double() / u.hp_max.to_double();
        const int legacy_band = std::min(3, int(std::floor((lf < 0.0 ? 0.0 : (lf > 1.0 ? 1.0 : lf)) * 7.0)) / 2);
        Color life = kLifeBand[kBandsOld ? (lf >= 0.75 ? 3 : lf >= 0.5 ? 2 : lf >= 0.25 ? 1 : 0) : legacy_band];
        if (o.team_colored) {
            life = u.player == o.viewer_player ? kTeamOwn
                 : (o.viewer_player >= 0 && w.are_allied(u.player, o.viewer_player)) ? kTeamAlly
                 : kTeamEnemy;
        }
        const double lt = (o.life_trail && u.id < o.life_trail->size()) ? double((*o.life_trail)[u.id]) : -1.0;
        draw_status_row(rgba, view_w, view_h, left, life_top, lf, life, segments_of(u.hp_max), kTrailOld ? lt : -1.0);
        int next_top = life_top + (kStatusBarHeight - 1);
        if (has_energy) {
            const double ef = u.energy.to_double() / u.energy_max.to_double();
            draw_status_row(rgba, view_w, view_h, left, next_top, ef, kEnergy, segments_of(u.energy_max));
            next_top += kStatusBarHeight - 1;
        }
        if (has_progress) {
            double pf = u.build_progress.to_double();
            pf = pf < 0.0 ? 0.0 : (pf > 1.0 ? 1.0 : pf);
            draw_status_row(rgba, view_w, view_h, left, next_top, pf, kProgress, 1);
            next_top += kStatusBarHeight - 1;
        }
        if (has_duration) {
            double df = u.time_life.to_double() / u.time_life_max.to_double();
            df = df < 0.0 ? 0.0 : (df > 1.0 ? 1.0 : df);
            draw_status_row(rgba, view_w, view_h, left, next_top, df, kDuration, 1);
        }
    }
    if (g_dirty_y0 <= g_dirty_y1) {
        dirty_y0 = g_dirty_y0;
        dirty_y1 = g_dirty_y1;
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
