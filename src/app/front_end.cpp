#include "sc2/app/front_end.hpp"
#include "sc2/ui/layout.hpp"
#include "sc2/app/shell_emblem.hpp"
#include "sc2/app/ui_canvas.hpp"
#include "sc2/app/ui_theme.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>

#include "sc2/app/ability.hpp"
#include "sc2/app/hud_layout.hpp"
#include "sc2/app/map_doodads.hpp"
#include "sc2/app/overlay_draw.hpp"
#include "sc2/app/race_melee_setup.hpp"
#include "sc2/app/shipping_wiring.hpp"
#include "sc2/app/spawn.hpp"
#include "sc2/app/water_heights.hpp"
#include "sc2/app/water_material.hpp"
#include "sc2/casc/storage.hpp"
#include "sc2/core/version.hpp"
#include "sc2/data/catalog.hpp"
#include "sc2/data/environment.hpp"
#include "sc2/render/creep_layer.hpp"
#include "sc2/data/economy.hpp"
#include "sc2/data/production.hpp"

namespace sc2::app {

using sim::Fixed;
using sim::Unit;

namespace {
Fixed Fx(float v) { return Fixed::from_ratio(int(std::lround(double(v) * 64.0)), 64); }

const char* race_name(Race r) {
    switch (r) {
        case Race::Protoss: return "Protoss";
        case Race::Zerg:    return "Zerg";
        case Race::Terran:
        default:            return "Terran";
    }
}

constexpr int kActWatch = 20, kReplayRow0 = 100000;
constexpr int kActStart = 1, kActBack = 2, kActMapsFolder = 3, kActScrollUp = 4,
              kActScrollDown = 5, kActP0Race = 10, kActP1Race = 11, kMapRow0 = 100;

constexpr const char* kMainMenuLayoutPath =
    "mods/core.sc2mod/base.sc2data/UI/Layout/Glue/ScreenNavigationSC2.SC2Layout";

const std::map<std::string, std::string>& nav_labels(casc::Storage& store, std::uint32_t w,
                                                     std::uint32_t h) {
    static std::map<std::pair<std::uint32_t, std::uint32_t>, std::map<std::string, std::string>>
        cache;
    const std::pair<std::uint32_t, std::uint32_t> key{w, h};
    std::map<std::pair<std::uint32_t, std::uint32_t>,
             std::map<std::string, std::string>>::iterator it = cache.find(key);
    if (it != cache.end()) return it->second;
    if (cache.size() >= 32) cache.clear();
    std::map<std::string, std::string> found;
    const ui::Rect canvas{0.0f, 0.0f, float(w), float(h)};
    if (std::optional<GlueScreen> g =
            GlueScreen::harvest(store, {"Glue/ScreenNavigationSC2.SC2Layout"}, canvas))
        for (const ui::SolvedFrame& f : g->controls())
            if (!f.text.empty()) found.emplace(f.name, f.text);
    return cache.emplace(key, std::move(found)).first->second;
}
}

namespace {

struct Board {
    std::vector<std::uint8_t>& out;
    std::uint32_t w, h;
    const render::OverlayFont* face[2] = {nullptr, nullptr};
    const std::map<std::string, theme::Rgb>* style_rgb = nullptr;
    int s(int authored) const { return theme::scaled(authored, h); }
    int rx(int authored) const { return int(w) - s(authored); }
    int by(int authored) const { return int(h) - s(authored); }
};

struct FaceScope {
    const render::OverlayFont* prev;
    FaceScope(const Board& b, theme::Face f) : prev(draw::active_font()) {
        const render::OverlayFont* want = b.face[std::size_t(f)];
        if (want && want->valid()) draw::set_active_font(want);
    }
    ~FaceScope() { draw::set_active_font(prev); }
};

int type_px(const Board& b, theme::Type t) { return std::max(1, b.s(t.px)); }
theme::Rgb role_rgb(const Board& b, theme::Type t, theme::Rgb c) {
    if (t.style && b.style_rgb) {
        auto it = b.style_rgb->find(t.style);
        if (it != b.style_rgb->end()) return it->second;
    }
    return c;
}

int text_at(const Board& b, int x, int y, const std::string& s, theme::Type t, theme::Rgb c,
            int cx1 = INT_MAX) {
    FaceScope fs(b, t.face);
    c = role_rgb(b, t, c);
    return draw::text_em(b.out, b.w, b.h, x, y, s, type_px(b, t), t.track, c.r, c.g, c.b, INT_MIN,
                         INT_MIN, cx1, INT_MAX);
}
int run_w(const Board& b, const std::string& s, theme::Type t) {
    FaceScope fs(b, t.face);
    return draw::text_w_em(s, type_px(b, t), t.track);
}
void text_box(const Board& b, int x0, int y0, int x1, int y1, const std::string& s, theme::Type t,
              theme::Rgb c) {
    FaceScope fs(b, t.face);
    c = role_rgb(b, t, c);
    const int px = type_px(b, t);
    const int tx = std::max(x0, x0 + (x1 - x0 - run_w(b, s, t)) / 2);
    const int ty = y0 + (y1 - y0 - draw::text_h_em(px)) / 2;
    draw::text_em(b.out, b.w, b.h, tx, ty, s, px, t.track, c.r, c.g, c.b, x0, INT_MIN, x1, INT_MAX);
}
void text_left(const Board& b, int x0, int y0, int y1, const std::string& s, theme::Type t,
               theme::Rgb c, int cx1) {
    FaceScope fs(b, t.face);
    c = role_rgb(b, t, c);
    const int px = type_px(b, t);
    draw::text_em(b.out, b.w, b.h, x0, y0 + (y1 - y0 - draw::text_h_em(px)) / 2, s, px, t.track,
                  c.r, c.g, c.b, INT_MIN, INT_MIN, cx1, INT_MAX);
}

void panel(const Board& b, int x0, int y0, int x1, int y1, theme::Rgb fill, std::uint8_t a,
           theme::Rgb border) {
    if (x1 <= x0 || y1 <= y0) return;
    draw::fill_a(b.out, b.w, b.h, x0, y0, x1, y1, fill.r, fill.g, fill.b, a);
    draw::box_a(b.out, b.w, b.h, x0, y0, x1, y1, border.r, border.g, border.b, 255);
}

void panel_grad(const Board& b, int x0, int y0, int x1, int y1, theme::Rgb top, theme::Rgb bottom,
                std::uint8_t a, theme::Rgb border) {
    if (x1 <= x0 || y1 <= y0) return;
    draw::vgrad(b.out, b.w, b.h, x0, y0, x1, y1, top.r, top.g, top.b, bottom.r, bottom.g, bottom.b,
                a);
    draw::box_a(b.out, b.w, b.h, x0, y0, x1, y1, border.r, border.g, border.b, 255);
}

void image_well(const Board& b, int x0, int y0, int x1, int y1) {
    panel(b, x0, y0, x1, y1, theme::kGroundInk, 255, theme::kBorderInset);
}

void image_well(const Board& b, int x0, int y0, int x1, int y1, const IconAtlas::Icon& img) {
    image_well(b, x0, y0, x1, y1);
    if (!img.ok() || x1 - x0 <= 2 || y1 - y0 <= 2) return;
    const int ix0 = x0 + 1, iy0 = y0 + 1, iw = x1 - x0 - 2, ih = y1 - y0 - 2;
    for (int y = 0; y < ih; ++y) {
        const int sy = int(std::int64_t(y) * img.h / ih);
        for (int x = 0; x < iw; ++x) {
            const int sx = int(std::int64_t(x) * img.w / iw);
            const std::uint8_t* p = &img.rgba[(std::size_t(sy) * img.w + std::size_t(sx)) * 4];
            if (p[3]) draw::fill_a(b.out, b.w, b.h, ix0 + x, iy0 + y, ix0 + x + 1, iy0 + y + 1, p[0], p[1], p[2], p[3]);
        }
    }
}

void button_menu(const Board& b, int x0, int y0, int x1, int y1, const std::string& label,
                 bool active, bool live = true) {
    if (x1 <= x0 || y1 <= y0) return;
    draw::vgrad(b.out, b.w, b.h, x0, y0, x1, y1, theme::kMenuBtnTop.r, theme::kMenuBtnTop.g,
                theme::kMenuBtnTop.b, theme::kMenuBtnBottom.r, theme::kMenuBtnBottom.g,
                theme::kMenuBtnBottom.b, 255);
    const theme::Rgb border = active ? theme::kAccent : theme::kBorderCtl;
    draw::box_a(b.out, b.w, b.h, x0, y0, x1, y1, border.r, border.g, border.b, 255);
    draw::inset_hi(b.out, b.w, b.h, x0, y0, x1, y1, theme::kInsetHi.r, theme::kInsetHi.g,
                   theme::kInsetHi.b, theme::kAlphaInsetBtn);
    const theme::Rgb label_c = active ? theme::kTextHi : theme::kTextBody;
    if (live) {
        text_box(b, x0, y0, x1, y1, label, theme::kTypeMenuBtn, label_c);
    } else {
        text_box(b, x0, y0, x1, y1, label, theme::kTypeMenuBtn, theme::kTextMuted2);
    }
}

void button_primary(const Board& b, int x0, int y0, int x1, int y1, const std::string& label,
                    theme::Type t) {
    if (x1 <= x0 || y1 <= y0) return;
    draw::vgrad(b.out, b.w, b.h, x0, y0, x1, y1, theme::kPrimaryTop.r, theme::kPrimaryTop.g,
                theme::kPrimaryTop.b, theme::kPrimaryBottom.r, theme::kPrimaryBottom.g,
                theme::kPrimaryBottom.b, 255);
    draw::box_a(b.out, b.w, b.h, x0, y0, x1, y1, theme::kBorderAccent.r, theme::kBorderAccent.g,
                theme::kBorderAccent.b, 255);
    draw::inset_hi(b.out, b.w, b.h, x0, y0, x1, y1, theme::kInsetPrimary.r, theme::kInsetPrimary.g,
                   theme::kInsetPrimary.b, theme::kAlphaInsetPrimary);
    text_box(b, x0, y0, x1, y1, label, t, theme::kTextOnPrimary);
}

void button_secondary(const Board& b, int x0, int y0, int x1, int y1, const std::string& label,
                      theme::Type t) {
    if (x1 <= x0 || y1 <= y0) return;
    panel(b, x0, y0, x1, y1, theme::kSurfaceRaised, 255, theme::kAccentDeep);
    text_box(b, x0, y0, x1, y1, label, t, theme::kTextBody);
}

void pulldown(const Board& b, int x0, int y0, int x1, int y1, const std::string& label,
              theme::Rgb label_c) {
    if (x1 <= x0 || y1 <= y0) return;
    panel(b, x0, y0, x1, y1, theme::kSurfaceCtl, 255, theme::kBorder);
    const int pad = b.s(theme::kPulldownPadX);
    const int caret_w = run_w(b, "v", theme::kTypeFine);
    text_left(b, x0 + pad, y0, y1, label, theme::kTypeSmall, label_c, x1 - pad - caret_w);
    text_left(b, x1 - pad - caret_w, y0, y1, "v", theme::kTypeFine, theme::kAccentDeep, x1);
}

void band_flow_strip(const Board& b, const std::string& title) {
    const int hgt = b.s(theme::kFlowStripH);
    draw::fill_a(b.out, b.w, b.h, 0, 0, int(b.w), hgt, theme::kBandGround.r, theme::kBandGround.g,
                 theme::kBandGround.b, theme::kAlphaStrip);
    draw::fill_a(b.out, b.w, b.h, 0, hgt - 1, int(b.w), hgt, theme::kBorderFaint.r,
                 theme::kBorderFaint.g, theme::kBorderFaint.b, 255);
    text_box(b, 0, 0, int(b.w), hgt, title, theme::kTypeScreenTitle, theme::kTextHi);
}

void band_nav(const Board& b) {
    const int hgt = b.s(theme::kNavBarH);
    draw::vgrad(b.out, b.w, b.h, 0, 0, int(b.w), hgt, theme::kNavTop.r, theme::kNavTop.g,
                theme::kNavTop.b, theme::kNavBottom.r, theme::kNavBottom.g, theme::kNavBottom.b,
                theme::kAlphaNavTop, theme::kAlphaNavBottom);
    draw::fill_a(b.out, b.w, b.h, 0, hgt - 1, int(b.w), hgt, theme::kBorder.r, theme::kBorder.g,
                 theme::kBorder.b, 255);
    draw::fill_a(b.out, b.w, b.h, 0, 0, int(b.w), 1, theme::kAccent.r, theme::kAccent.g,
                 theme::kAccent.b, 128);
}

void band_footer(const Board& b) {
    const int y0 = b.by(theme::kFooterH);
    draw::fill_a(b.out, b.w, b.h, 0, y0, int(b.w), int(b.h), theme::kBandGround.r,
                 theme::kBandGround.g, theme::kBandGround.b, theme::kAlphaBand);
    draw::fill_a(b.out, b.w, b.h, 0, y0, int(b.w), y0 + 1, theme::kBorderFaint.r,
                 theme::kBorderFaint.g, theme::kBorderFaint.b, 255);
    const int pad = b.s(theme::kPagePad);
    const int mark = b.s(theme::kFooterMark);
    const int my = y0 + (b.s(theme::kFooterH) - mark) / 2;
    image_well(b, pad, my, pad + mark, my + mark, shell_emblem(true));
    const std::string v = std::string("crafter-sc ") + sc2::kVersion;
    text_left(b, pad + mark + b.s(8), y0, int(b.h), v, theme::kTypeSmall, theme::kAccentDeep,
              int(b.w) - pad);
}

void mark_version(const Board& b) {
    const int mark = b.s(theme::kFooterMark);
    const std::string v = std::string("crafter-sc ") + sc2::kVersion;
    const int tw = run_w(b, v, theme::kTypeMicro);
    const int x1 = b.rx(theme::kScreenPad);
    const int y1 = b.by(16);
    image_well(b, x1 - tw - b.s(8) - mark, y1 - mark, x1 - tw - b.s(8), y1, shell_emblem(true));
    text_left(b, x1 - tw, y1 - mark, y1, v, theme::kTypeMicro, theme::kAccentDeep, x1);
}

void background(const Board& b, theme::Rgb inner, theme::Rgb ground) {
    draw::fill(b.out, b.w, b.h, 0, 0, int(b.w), int(b.h), ground.r, ground.g, ground.b, 255);
    draw::vgrad(b.out, b.w, b.h, 0, 0, int(b.w), int(b.h), inner.r, inner.g, inner.b, ground.r,
                ground.g, ground.b, 255);
}

void wordmark(const Board& b, int x, int y, theme::Type top, theme::Type bottom) {
    const int tpx = type_px(b, top);
    text_at(b, x, y, "CRAFTER", top, theme::kTextHi);
    text_at(b, x, y + draw::text_h_em(tpx) + b.s(2), "SC", bottom, theme::kAccentPure);
}

struct Ctl {
    std::string label;
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    int action = 0;
    bool live = false;
    bool selected = false;
};

void compose_controls(const Board& b, const std::vector<Ctl>& ctls) {
    for (const Ctl& c : ctls) button_menu(b, c.x0, c.y0, c.x1, c.y1, c.label, false, c.live);
}

void draw_shell_root(const Board& b, const std::vector<Ctl>& ctls) {
    band_nav(b);
    const int navh = b.s(theme::kNavBarH);

    const int ex0 = b.s(theme::kMainEmblemLeft), ey0 = b.s(theme::kMainEmblemTop);
    const int esz = b.s(theme::kEmblemLarge);
    image_well(b, ex0, ey0, ex0 + esz, ey0 + esz, shell_emblem(false));
    const int wx = ex0 + esz + b.s(theme::kEmblemGapLarge);
    const int wh = draw::text_h_em(type_px(b, theme::kTypeWordmark)) * 2 + b.s(2);
    wordmark(b, wx, ey0 + (esz - wh) / 2, theme::kTypeWordmark, theme::kTypeWordmarkAccent);
    draw::fill_a(b.out, b.w, b.h, wx, ey0 + (esz + wh) / 2 + b.s(12), wx + b.s(theme::kEmblemLarge),
                 ey0 + (esz + wh) / 2 + b.s(12) + b.s(theme::kTabRuleH), theme::kAccentPure.r,
                 theme::kAccentPure.g, theme::kAccentPure.b, 255);

    {
        const std::string ver = "v" + std::to_string(sc2::kVersionMajor) + "." +
                                std::to_string(sc2::kVersionMinor) + "." +
                                std::to_string(sc2::kVersionPatch) + "." + sc2::kVersionBuild;
        std::string stage = sc2::kVersionStage;
        for (char& ch : stage)
            if (ch >= 'a' && ch <= 'z') ch = char(ch - 'a' + 'A');
        const std::string data_line = std::string("crafter-sc ") + sc2::kVersion;
        const int padx = b.s(theme::kVersionPadX), pady = b.s(theme::kVersionPadY);
        const int wv = std::max(run_w(b, ver, theme::kTypeVersionBig),
                                run_w(b, data_line, theme::kTypeVersionFine)) +
                       padx * 2;
        const int hv = draw::text_h_em(type_px(b, theme::kTypeVersionBig)) +
                       draw::text_h_em(type_px(b, theme::kTypeVersionStage)) +
                       draw::text_h_em(type_px(b, theme::kTypeVersionFine)) + pady * 2 + b.s(8);
        const int x1 = b.rx(theme::kVersionRight), y1 = b.by(theme::kVersionBottom);
        const int x0 = x1 - wv, y0 = y1 - hv;
        draw::fill_a(b.out, b.w, b.h, x0, y0, x1, y1, theme::kBandGround.r, theme::kBandGround.g,
                     theme::kBandGround.b, theme::kAlphaVersion);
        draw::box_a(b.out, b.w, b.h, x0, y0, x1, y1, theme::kBorderVersion.r,
                    theme::kBorderVersion.g, theme::kBorderVersion.b, 255);
        draw::fill_a(b.out, b.w, b.h, x1 - b.s(theme::kVersionRimW), y0, x1, y1, theme::kAccent.r,
                     theme::kAccent.g, theme::kAccent.b, 255);
        int ty = y0 + pady;
        text_at(b, x1 - padx - run_w(b, ver, theme::kTypeVersionBig), ty, ver,
                theme::kTypeVersionBig, theme::kTextHi);
        ty += draw::text_h_em(type_px(b, theme::kTypeVersionBig)) + b.s(2);
        text_at(b, x1 - padx - run_w(b, stage, theme::kTypeVersionStage), ty, stage,
                theme::kTypeVersionStage, theme::kAccent);
        ty += draw::text_h_em(type_px(b, theme::kTypeVersionStage)) + b.s(6);
        text_at(b, x1 - padx - run_w(b, data_line, theme::kTypeVersionFine), ty, data_line,
                theme::kTypeVersionFine, theme::kTextFaint);
    }

    for (const Ctl& c : ctls) {
        if (c.y1 <= 0 || c.x1 <= c.x0) continue;
        const bool in_nav = (c.y0 + c.y1) / 2 < navh;
        if (in_nav) {
            const int dh = b.s(theme::kNavDividerH);
            const int dy = (navh - dh) / 2;
            draw::fill_a(b.out, b.w, b.h, c.x0, dy, c.x0 + 1, dy + dh, theme::kBorderCtl.r,
                         theme::kBorderCtl.g, theme::kBorderCtl.b, 255);
            text_box(b, c.x0, c.y0, c.x1, c.y1, c.label, theme::kTypeNavItem,
                     c.live ? theme::kTextBody : theme::kTextMuted2);
        } else {
            button_menu(b, c.x0, c.y0, c.x1, c.y1, c.label, false, c.live);
        }
    }
    band_footer(b);
}

void draw_home(const Board& b, const std::vector<Ctl>& ctls) {
    band_flow_strip(b, "HOME");
    const int ex0 = b.s(theme::kHomeLeft), ey0 = b.s(theme::kHomeEmblemTop);
    const int esz = b.s(theme::kEmblemSmall);
    image_well(b, ex0, ey0, ex0 + esz, ey0 + esz, shell_emblem(false));
    const int wx = ex0 + esz + b.s(theme::kEmblemGapSmall);
    const int wh = draw::text_h_em(type_px(b, theme::kTypeWordmarkSm)) * 2 + b.s(2);
    wordmark(b, wx, ey0 + (esz - wh) / 2, theme::kTypeWordmarkSm, theme::kTypeWordmarkSmAccent);
    compose_controls(b, ctls);
    band_footer(b);
}

void draw_custom(const Board& b, const std::vector<Ctl>& ctls) {
    band_flow_strip(b, "CUSTOM GAMES - MAP SELECTION");
    const int pad = b.s(theme::kScreenPad);
    const int tabs_y = b.s(theme::kTabsTop);
    const int tab_h = b.s(theme::kTabPadY) * 2 + draw::text_h_em(type_px(b, theme::kTypeTab));
    draw::fill_a(b.out, b.w, b.h, pad, tabs_y + tab_h, int(b.w) - pad, tabs_y + tab_h + 1,
                 theme::kBorderFaint.r, theme::kBorderFaint.g, theme::kBorderFaint.b, 255);
    {
        const int tw = b.s(theme::kTabPadX) * 2 + run_w(b, "MELEE", theme::kTypeTab);
        text_box(b, pad, tabs_y, pad + tw, tabs_y + tab_h, "MELEE", theme::kTypeTab,
                 theme::kTextHi);
        draw::fill_a(b.out, b.w, b.h, pad, tabs_y + tab_h - b.s(theme::kTabRuleH), pad + tw,
                     tabs_y + tab_h, theme::kAccent.r, theme::kAccent.g, theme::kAccent.b, 255);
    }
    const int top = b.s(theme::kCustomListTop);
    const int bot = b.by(theme::kCustomBottomGap);
    const int detail_x0 = b.rx(theme::kScreenPad) - b.s(theme::kCustomDetailW);
    panel(b, pad, top, pad + b.s(theme::kCustomListW), bot, theme::kSurface, theme::kAlphaPanelDim,
          theme::kBorderFaint);
    panel(b, detail_x0, top, b.rx(theme::kScreenPad), bot, theme::kSurface, theme::kAlphaPanel,
          theme::kBorder);
    const int pv = b.s(theme::kCustomPreview), dp = b.s(theme::kCustomDetailPad);
    image_well(b, detail_x0 + dp, top + dp, detail_x0 + dp + pv, top + dp + pv);
    compose_controls(b, ctls);
    band_footer(b);
}

void draw_options(const Board& b, const std::vector<Ctl>& ctls, int documents, int selected_rail) {
    band_flow_strip(b, "OPTIONS");
    const int dw = b.s(theme::kOptionsW), dh = b.s(theme::kOptionsH);
    const int x0 = (int(b.w) - dw) / 2, y0 = b.s(theme::kOptionsTop);
    const int x1 = x0 + dw, y1 = y0 + dh;
    panel(b, x0, y0, x1, y1, theme::kSurface, 255, theme::kBorderStrong);
    const int rail = x0 + b.s(theme::kOptionsRailW);
    draw::fill_a(b.out, b.w, b.h, rail, y0 + 1, rail + 1, y1 - 1, theme::kSurfaceHi.r,
                 theme::kSurfaceHi.g, theme::kSurfaceHi.b, 255);
    static const char* const kPanels[] = {"GAMEPLAY", "GRAPHICS", "VIDEO",    "SOUND",
                                          "VOICE",    "INPUT",    "HOTKEYS",  "COLOR",
                                          "LANGUAGE", "OBSERVER", "SOCIAL",   "STEREOSCOPIC"};
    const int cat_h =
        b.s(theme::kCatPadY) * 2 + draw::text_h_em(type_px(b, theme::kTypeCat));
    int cy = y0 + b.s(theme::kOptionsRailPadY);
    for (int i = 0; i < int(sizeof(kPanels) / sizeof(kPanels[0])); ++i) {
        if (i == selected_rail) {
            draw::fill_a(b.out, b.w, b.h, x0 + 1, cy, rail, cy + cat_h, theme::kSurfaceSel.r,
                         theme::kSurfaceSel.g, theme::kSurfaceSel.b, theme::kAlphaCatActive);
            draw::fill_a(b.out, b.w, b.h, x0 + 1, cy, x0 + 1 + b.s(theme::kCatRuleW), cy + cat_h,
                         theme::kAccent.r, theme::kAccent.g, theme::kAccent.b, 255);
        }
        text_left(b, x0 + b.s(theme::kCatPadX), cy, cy + cat_h, kPanels[i], theme::kTypeCat,
                  i == selected_rail ? theme::kTextHi : theme::kTextMuted, rail);
        cy += cat_h;
    }
    band_footer(b);
    compose_controls(b, ctls);
    const std::string note = std::to_string(ctls.size()) + " controls: the OptionsDialog + its " +
                             kPanels[selected_rail < 0 ? 0 : selected_rail] + " panel (of " + std::to_string(documents - 1) +
                             " panel documents, one shown at a time)";
    text_at(b, b.s(theme::kScreenPad), b.by(theme::kFooterH + theme::kPagePad), note,
            theme::kTypeCaption, theme::kTextMuted, int(b.w) - b.s(theme::kScreenPad));
}

void draw_pause(const Board& b, const std::vector<Ctl>& ctls) {
    draw::fill_a(b.out, b.w, b.h, 0, 0, int(b.w), int(b.h), 0, 0, 0, theme::kAlphaScrim);
    const int dw = b.s(theme::kDialogW);
    const int btn_h = b.s(theme::kMenuBtnH), gap = b.s(theme::kMenuBtnGap);
    const int title_h = draw::text_h_em(type_px(b, theme::kTypeDialogTitle));
    const int n = int(ctls.size());
    const int dh = b.s(theme::kDialogPadTop) + title_h + b.s(theme::kDialogTitleGap) +
                   (n > 0 ? n * btn_h + (n - 1) * gap : 0) + b.s(theme::kDialogPadBottom);
    const int x0 = (int(b.w) - dw) / 2, y0 = (int(b.h) - dh) / 2;
    panel_grad(b, x0, y0, x0 + dw, y0 + dh, theme::kSurfaceRaised, theme::kSurface, 255,
               theme::kBorderStrong);
    text_box(b, x0, y0 + b.s(theme::kDialogPadTop), x0 + dw,
             y0 + b.s(theme::kDialogPadTop) + title_h, "GAME MENU", theme::kTypeDialogTitle,
             theme::kTextHi);
    compose_controls(b, ctls);
    mark_version(b);
}

struct LoadInfo {
    std::string map_name;
    std::string p0_race, p1_race;
    theme::Rgb p0_color, p1_color;
    bool have_colors = false;
    int done = 0, total = 0;
    std::string stage;
};
void draw_loading(const Board& b, const LoadInfo& info) {
    band_flow_strip(b, "LOADING");
    const int ty = b.s(theme::kLoadTitleTop);
    text_box(b, 0, ty, int(b.w), ty + draw::text_h_em(type_px(b, theme::kTypeLoadTitle)),
             info.map_name, theme::kTypeLoadTitle, theme::kTextHi);
    text_box(b, 0, ty + draw::text_h_em(type_px(b, theme::kTypeLoadTitle)) + b.s(6), int(b.w),
             ty + draw::text_h_em(type_px(b, theme::kTypeLoadTitle)) + b.s(6) +
                 draw::text_h_em(type_px(b, theme::kTypeScreenTitle)),
             "1V1 - MELEE", theme::kTypeScreenTitle, theme::kTextMuted);

    const int plate = b.s(theme::kLoadPlate), gap = b.s(theme::kLoadPlateGap);
    const int vsw = run_w(b, "VS", theme::kTypeVs);
    const int side = plate + gap + b.s(theme::kLoadPlayerBarW);
    const int total = side * 2 + vsw + b.s(theme::kLoadVsGap) * 2;
    int x = (int(b.w) - total) / 2;
    const int vy = b.s(theme::kLoadVsTop);
    auto side_block = [&](int px0, const std::string& race, theme::Rgb col, bool mirror) {
        const int pl_x = mirror ? px0 + b.s(theme::kLoadPlayerBarW) + gap : px0;
        panel(b, pl_x, vy, pl_x + plate, vy + plate, theme::kSurfaceCtl, 255, col);
        const int tx = mirror ? px0 : px0 + plate + gap;
        text_at(b, tx, vy + b.s(8), race, theme::kTypeSlotName, theme::kTextBody);
        const int bary = vy + b.s(8) + draw::text_h_em(type_px(b, theme::kTypeSlotName)) + b.s(8);
        panel(b, tx, bary, tx + b.s(theme::kLoadPlayerBarW), bary + b.s(theme::kLoadPlayerBarH),
              theme::kMenuBtnBottom, 255, theme::kBorderInset);
    };
    side_block(x, info.p0_race, info.have_colors ? info.p0_color : theme::kBorderSel, false);
    x += side + b.s(theme::kLoadVsGap);
    text_at(b, x, vy + (plate - draw::text_h_em(type_px(b, theme::kTypeVs))) / 2, "VS",
            theme::kTypeVs, theme::kBorderCtl);
    x += vsw + b.s(theme::kLoadVsGap);
    side_block(x, info.p1_race, info.have_colors ? info.p1_color : theme::kBorderSel, true);

    const int bw = b.s(theme::kLoadBarW), bh = b.s(theme::kLoadBarH);
    const int bx0 = (int(b.w) - bw) / 2, by0 = b.by(theme::kLoadBarBottom);
    panel(b, bx0, by0, bx0 + bw, by0 + bh, theme::kSurfaceDim, 255, theme::kBorder);
    if (info.total > 0) {
        const int fill = int(std::int64_t(bw) * info.done / info.total);
        if (fill > 0)
            panel(b, bx0, by0, bx0 + fill, by0 + bh, theme::kBorderSel, 255, theme::kBorderSel);
        text_at(b, bx0, by0 + bh + b.s(6),
                info.stage + "  -  " + std::to_string(info.done) + " / " +
                    std::to_string(info.total),
                theme::kTypeCaption, theme::kTextFaint, bx0 + bw);
    }
    text_box(b, 0, b.by(theme::kLoadTipBottom), int(b.w),
             b.by(theme::kLoadTipBottom) + draw::text_h_em(type_px(b, theme::kTypeBody)),
             "TIP: Hold Alt to see the life bars of every unit on screen.", theme::kTypeBody,
             theme::kTextMuted);
    band_footer(b);
}

struct LobbyInfo {
    std::string selected;
    std::string count_line;
    std::string error;
};
void draw_lobby(const Board& b, const std::vector<Ctl>& ctls, const LobbyInfo& info, int act_start,
                int act_back, int act_folder, int map_row0) {
    band_nav(b);
    const int pad = b.s(theme::kScreenPad);
    text_at(b, pad, b.s(14), "GAME LOBBY", theme::kTypePanelTitle, theme::kTextHi);

    const int slots_y = b.s(theme::kLobbySlotsTop);
    const int slot_h = b.s(theme::kPulldownH) + b.s(theme::kSlotPadY) * 2;
    const int slots_w = b.s(theme::kLobbySlotsW);
    for (int i = 0; i < 2; ++i) {
        const int ry = slots_y + i * (slot_h + b.s(theme::kSlotRowGap));
        panel(b, pad, ry, pad + slots_w, ry + slot_h, theme::kSurfaceRow, theme::kAlphaPanelDim,
              i == 0 ? theme::kBorderSlot : theme::kBorderFaint);
        pulldown(b, pad + b.s(theme::kSlotPadX), ry + b.s(theme::kSlotPadY),
                 pad + b.s(theme::kSlotPadX) + b.s(theme::kColPlayer),
                 ry + b.s(theme::kSlotPadY) + b.s(theme::kPulldownH),
                 i == 0 ? "You (Host)" : "A.I. SLOT", theme::kTextBody);
    }
    const int band_y = b.s(theme::kLobbyOptionsTop);
    panel(b, pad, band_y, pad + slots_w, b.by(theme::kFooterH) - b.s(theme::kPagePad),
          theme::kSurface, theme::kAlphaPanelDim, theme::kBorderFaint);

    const int mi_x1 = b.rx(theme::kScreenPad), mi_x0 = mi_x1 - b.s(theme::kMapInfoW);
    const int mi_h = b.s(theme::kMinimapBox) + b.s(theme::kMapInfoPad) * 2;
    panel(b, mi_x0, slots_y, mi_x1, slots_y + mi_h, theme::kSurface, theme::kAlphaPanel,
          theme::kBorder);
    const int mm = b.s(theme::kMinimapBox), mp = b.s(theme::kMapInfoPad);
    image_well(b, mi_x0 + mp, slots_y + mp, mi_x0 + mp + mm, slots_y + mp + mm);
    const int inf_x = mi_x0 + mp + mm + mp;
    text_at(b, inf_x, slots_y + mp, info.selected, theme::kTypeDetailTitle, theme::kTextBody,
            mi_x1 - mp);
    text_at(b, inf_x, slots_y + mp + b.s(30), info.count_line, theme::kTypeMicro,
            theme::kTextMuted, mi_x1 - mp);
    if (!info.error.empty())
        text_at(b, inf_x, slots_y + mp + b.s(52), info.error, theme::kTypeMicro, theme::kErr,
                mi_x1 - mp);

    for (const Ctl& c : ctls) {
        if (c.x1 <= c.x0 || c.y1 <= c.y0) continue;
        if (c.action == act_start) {
            button_primary(b, c.x0, c.y0, c.x1, c.y1, c.label, theme::kTypePrimaryLg);
        } else if (c.action == act_back) {
            button_secondary(b, c.x0, c.y0, c.x1, c.y1, c.label, theme::kTypeSecondaryLg);
        } else if (c.action == act_folder) {
            button_secondary(b, c.x0, c.y0, c.x1, c.y1, c.label, theme::kTypeSecondary);
        } else if (c.action >= map_row0) {
            panel(b, c.x0, c.y0, c.x1, c.y1, c.selected ? theme::kSurfaceSel : theme::kSurfaceRow,
                  theme::kAlphaPanelDim, c.selected ? theme::kAccent : theme::kBorderFaint);
            text_left(b, c.x0 + b.s(theme::kMapRowPadX), c.y0, c.y1, c.label, theme::kTypeSmall,
                      c.selected ? theme::kTextHi : theme::kTextCell,
                      c.x1 - b.s(theme::kMapRowPadX));
        } else {
            pulldown(b, c.x0, c.y0, c.x1, c.y1, c.label, theme::kTextBody);
        }
    }
    mark_version(b);
}

}

FrontEnd::FrontEnd(data::Catalog& cat, MeleeModels& models, casc::Storage& store,
                   const TestMap& map)
    : cat_(cat), models_(models), store_(store), map_(map) {
    menu_ctl_.focus = {map_.width * 0.5f, map_.height * 0.5f, 0.0f};
    menu_ctl_.distance = 48.0f;
    menu_ctl_.yaw = 1.5707963f;
    menu_ctl_.pitch = 1.0f;
    menu_ctl_.mirror_x = true;
    maps_.push_back({"", "Synthetic Arena (72x72)  [built-in, no cliffs]"});
}

void FrontEnd::set_maps(std::vector<platform::MapEntry> maps) {
    maps_.assign(1, platform::MapEntry{"", "Synthetic Arena (72x72)  [built-in, no cliffs]"});
    for (auto& m : maps) maps_.push_back(std::move(m));
    sel_map_ = maps_.size() > 1 ? 1 : 0;
    map_scroll_ = 0;
}

int FrontEnd::lobby_rows_per_page(std::uint32_t view_h) const {
    const int pitch = theme::scaled(theme::kPulldownH, view_h) +
                      theme::scaled(theme::kSlotRowGap, view_h);
    const int top = theme::scaled(theme::kLobbyOptionsTop, view_h) +
                    theme::scaled(theme::kMapRowPadY, view_h);
    const int bottom = int(view_h) - theme::scaled(theme::kFooterH, view_h) -
                       theme::scaled(theme::kPagePad, view_h) - pitch * 2;
    return std::max(1, (bottom - top) / std::max(1, pitch));
}

void FrontEnd::clamp_map_scroll(std::uint32_t view_h) {
    const int page = lobby_rows_per_page(view_h);
    const int max_scroll = std::max(0, int(maps_.size()) - page);
    map_scroll_ = std::max(0, std::min(map_scroll_, max_scroll));
}

const MainMenu* FrontEnd::real_menu(std::uint32_t view_w, std::uint32_t view_h) {
    if (real_menu_disabled_ || !view_w || !view_h) return nullptr;
    if (real_menu_forced_) return real_menu_ ? &*real_menu_ : nullptr;
    if (real_menu_tried_ && real_menu_w_ == view_w && real_menu_h_ == view_h)
        return real_menu_ ? &*real_menu_ : nullptr;
    real_menu_tried_ = true;
    real_menu_w_ = view_w;
    real_menu_h_ = view_h;
    real_menu_.reset();

    std::string dir = menu_data_dir_;
    if (dir.empty())
        if (const char* env = std::getenv("SC2_DATA_DIR")) dir = env;
    if (dir.empty()) return nullptr;

    const float ref_scale = float(view_h) / app::kRefHeight;
    const ui::Rect canvas{0, 0, app::kRefHeight * float(view_w) / float(view_h), app::kRefHeight};
    std::optional<MainMenu> m = MainMenu::from_store(store_, kMainMenuLayoutPath, canvas);
    if (!m) return nullptr;
    m->scale(ref_scale);

    const float canvas_area = float(view_w) * float(view_h);
    bool usable = false;
    for (const ui::SolvedFrame& sf : m->buttons()) {
        if (MainMenu::classify(sf.name) == MenuAction::None) continue;
        if (sf.rect.w <= 1.0f || sf.rect.h <= 1.0f) continue;
        if (sf.rect.w * sf.rect.h < 0.9f * canvas_area) { usable = true; break; }
    }
    if (!usable) return nullptr;
    real_menu_ = std::move(m);
    return &*real_menu_;
}

std::vector<FrontEnd::Button> FrontEnd::menu_buttons(std::uint32_t view_w, std::uint32_t view_h) {
    std::vector<Button> out;
    if (const MainMenu* m = real_menu(view_w, view_h)) {
        const std::map<std::string, std::string>& resolved =
            nav_labels(store_, view_w, view_h);
        out.reserve(m->buttons().size());
        for (std::size_t i = 0; i < m->buttons().size(); ++i) {
            const ui::Rect& r = m->hit_rect(i);
            const std::string& name = m->buttons()[i].name;
            std::string text = m->buttons()[i].text;
            if (text.empty()) {
                const std::map<std::string, std::string>::const_iterator it = resolved.find(name);
                if (it != resolved.end()) text = it->second;
            }
            out.push_back({int(r.x), int(r.y), int(r.x + r.w), int(r.y + r.h),
                           text.empty() ? name : text, name, 0});
        }
    }

    struct OwnEntry { const char* label; const char* trigger; };
    static const OwnEntry kOwn[] = {
        {"1v1 Melee  vs AI", "PlayButton"},
        {"Sandbox  Test Map", "SandboxButton"},
        {"Options", "menu/Options"},
        {"Credits", "Credits entry"},
    };
    const int bw = theme::scaled(theme::kMenuBtnW, view_h);
    const int bh = theme::scaled(theme::kMenuBtnH, view_h);
    const int gap = theme::scaled(theme::kMenuBtnGap, view_h);
    const int x0 = theme::scaled(theme::kMainEmblemLeft, view_h);
    int y = theme::scaled(theme::kMainContentTop, view_h);
    for (const Button& b : out)
        if (b.y1 + gap > y && b.x1 > x0 && b.x0 < x0 + bw) y = b.y1 + gap;
    for (const OwnEntry& e : kOwn) {
        bool already = false;
        for (const Button& b : out)
            if (trigger_for_control(ScreenId::GlueRoot, b.trigger) == e.trigger) already = true;
        if (already) continue;
        out.push_back({x0, y, x0 + bw, y + bh, e.label, e.trigger, 0});
        y += bh + gap;
    }
    return out;
}

const GlueScreen* FrontEnd::glue_screen(ScreenId id, std::uint32_t view_w, std::uint32_t view_h) {
    if (id >= ScreenId::Count || !view_w || !view_h) return nullptr;
    GlueCacheEntry& e = glue_cache_[std::size_t(id)];
    if (e.tried && e.w == view_w && e.h == view_h) return e.screen ? &*e.screen : nullptr;
    e.tried = true;
    e.w = view_w;
    e.h = view_h;
    e.screen.reset();
    const std::vector<const char*>& docs = screen_layouts(id);
    if (docs.empty()) return nullptr;
    std::vector<std::string> paths(docs.begin(), docs.end());
    GlueScreen::HarvestOptions opts;
    if (id == ScreenId::Options) {
        opts.screen_docs = 1;
        const std::vector<const char*>& panels = options_panels();
        if (options_panel_ >= 0 && std::size_t(options_panel_) < panels.size()) opts.shown.push_back(panels[std::size_t(options_panel_)]);
    }
    e.screen = GlueScreen::harvest(store_, paths, ui::Rect{0, 0, app::kRefHeight * float(view_w) / float(view_h), app::kRefHeight}, opts);
    if (e.screen) e.screen->scale(float(view_h) / app::kRefHeight);
    return e.screen ? &*e.screen : nullptr;
}

const std::vector<const char*>& FrontEnd::options_panels() {
    static const std::vector<const char*> k = {"GameplayOptionsPanel", "GraphicsOptionsPanel",   "VideoOptionsPanel",
                                               "SoundOptionsPanel",    "VoiceOptionsPanel",      "InputOptionsPanel",
                                               "ColorOptionsPanel",    "LanguageOptionsPanel",   "ObserverOptionsPanel",
                                               "SocialOptionsPanel",   "StereoscopicOptionsPanel"};
    return k;
}

void FrontEnd::select_options_panel(int i) {
    if (i < 0 || std::size_t(i) >= options_panels().size() || i == options_panel_) return;
    options_panel_ = i;
    glue_cache_[std::size_t(ScreenId::Options)].tried = false;
}

void FrontEnd::click_glue_screen(ScreenId id, float mx, float my, std::uint32_t view_w,
                                 std::uint32_t view_h) {
    const GlueScreen* s = glue_screen(id, view_w, view_h);
    if (!s) return;
    const ui::SolvedFrame* f = s->hit(mx, my);
    if (!f) return;
    if (id == ScreenId::Options && f->name.size() > 6 && f->name.compare(f->name.size() - 6, 6, "Button") == 0) {
        const std::string panel = f->name.substr(0, f->name.size() - 6) + "OptionsPanel";
        const std::vector<const char*>& panels = options_panels();
        for (std::size_t i = 0; i < panels.size(); ++i)
            if (panel == panels[i]) {
                select_options_panel(int(i));
                return;
            }
    }
    if (id == ScreenId::Options && f->name.size() > 8 && f->name.compare(f->name.size() - 8, 8, "CheckBox") == 0) {
        toggle_option(f->name);
        return;
    }
    router_.follow(trigger_for_control(id, f->name));
}

void FrontEnd::capture_score() {
    if (!skirmish_) return;
    const sim::ScoreKeeper& sk = skirmish_->score();
    struct Def { const char* id; const char* publish; std::int32_t sim::PlayerScore::*field; };
    static const Def kRows[] = {
        {"MineralsCurrent", "MineralsCurrent", &sim::PlayerScore::minerals},
        {"VespeneCurrent", "VespeneCurrent", &sim::PlayerScore::gas},
        {"UnitsProduced", "Units Produced", &sim::PlayerScore::units_produced},
        {"UnitsLostCount", "Lost Unit Count", &sim::PlayerScore::units_lost},
        {"StructuresBuiltCount", "Structures Built", &sim::PlayerScore::structures_built},
        {"StructuresLostCount", "Structures Lost Count", &sim::PlayerScore::structures_lost},
    };
    last_score_.clear();
    for (const Def& d : kRows) {
        ScoreRow r;
        r.id = d.id;
        r.publish_name = d.publish;
        for (int p = 0; p < 2; ++p) r.value[p] = std::size_t(p) < sk.players() ? sk.player(p).*d.field : 0;
        last_score_.push_back(std::move(r));
    }
}

void FrontEnd::draw_score_table(std::uint32_t view_w, std::uint32_t view_h, std::vector<std::uint8_t>& out) const {
    if (last_score_.empty() || out.size() < std::size_t(view_w) * view_h * 4) return;
    Board b{out, view_w, view_h};
    b.face[0] = &overlay_font_;
    b.face[1] = &overlay_font_ext_;
    b.style_rgb = &style_rgb_;
    const int row_h = b.s(36), pad = b.s(theme::kScreenPad);
    const int x0 = pad, x1 = int(view_w) - pad;
    const int y0 = b.s(theme::kFlowStripH) + b.s(theme::kPagePad) + b.s(60);
    const int rows = int(last_score_.size()) + 1;
    panel(b, x0, y0, x1, y0 + rows * row_h + b.s(theme::kPagePad), theme::kSurface, theme::kAlphaPanel, theme::kBorderStrong);
    const int col_w = (x1 - x0) / 4;
    const int cx1 = x0 + 2 * col_w, cx2 = x0 + 3 * col_w;
    int y = y0 + b.s(theme::kPagePad) / 2;
    text_left(b, x0 + pad, y, y + row_h, "SCORE", theme::kTypeSectionHead, theme::kAccent, x1);
    text_box(b, cx1, y, cx1 + col_w, y + row_h, std::string("Player 1 (") + lobby_race_name(p0_pick_) + ")", theme::kTypeSubHead, theme::kTextHi);
    text_box(b, cx2, y, cx2 + col_w, y + row_h, std::string("Player 2 (") + lobby_race_name(p1_pick_) + ")", theme::kTypeSubHead, theme::kTextHi);
    for (const ScoreRow& r : last_score_) {
        y += row_h;
        draw::fill_a(b.out, b.w, b.h, x0 + 1, y, x1 - 1, y + 1, theme::kBorderRow.r, theme::kBorderRow.g, theme::kBorderRow.b, 255);
        text_left(b, x0 + pad, y, y + row_h, r.publish_name, theme::kTypeBody, theme::kTextBody, cx1);
        text_box(b, cx1, y, cx1 + col_w, y + row_h, std::to_string(r.value[0]), theme::kTypeBody, theme::kTextCell);
        text_box(b, cx2, y, cx2 + col_w, y + row_h, std::to_string(r.value[1]), theme::kTypeBody, theme::kTextCell);
    }
}

std::string FrontEnd::option_key(const std::string& checkbox_name) {
    static const std::string kSuffix = "CheckBox";
    std::string base = checkbox_name;
    if (base.size() > kSuffix.size() && base.compare(base.size() - kSuffix.size(), kSuffix.size(), kSuffix) == 0)
        base.resize(base.size() - kSuffix.size());
    return "options." + base;
}
bool FrontEnd::option_checked(const std::string& checkbox_name) const {
    const std::string key = option_key(checkbox_name);
    std::string v;
    if (config_get_) v = config_get_(key);
    else {
        auto it = config_mem_.find(key);
        if (it != config_mem_.end()) v = it->second;
    }
    return v == "1" || v == "true" || v == "on";
}
void FrontEnd::toggle_option(const std::string& checkbox_name) {
    const std::string key = option_key(checkbox_name);
    const std::string v = option_checked(checkbox_name) ? "0" : "1";
    if (config_set_) config_set_(key, v);
    else config_mem_[key] = v;
}

std::vector<FrontEnd::Button> FrontEnd::lobby_buttons(std::uint32_t view_w,
                                                      std::uint32_t view_h) const {
    std::vector<Button> out;
    const int W = int(view_w), H = int(view_h);
    auto S = [view_h](int authored) { return theme::scaled(authored, view_h); };

    const int pad = S(theme::kScreenPad);
    const int slots_w = S(theme::kLobbySlotsW);
    const int pd_h = S(theme::kPulldownH);
    const int pitch = pd_h + S(theme::kSlotRowGap);

    const int slot_h = pd_h + S(theme::kSlotPadY) * 2;
    const int race_x0 = pad + S(theme::kSlotPadX) + S(theme::kColPlayer) + S(theme::kSlotGapX);
    const int race_x1 = race_x0 + S(theme::kColRace);
    for (int i = 0; i < 2; ++i) {
        const int ry = S(theme::kLobbySlotsTop) + i * (slot_h + S(theme::kSlotRowGap)) +
                       S(theme::kSlotPadY);
        out.push_back({race_x0, ry, race_x1, ry + pd_h,
                       lobby_race_name(i == 0 ? p0_pick_ : p1_pick_), "",
                       i == 0 ? kActP0Race : kActP1Race});
    }

    const int lx0 = pad + S(theme::kMapRowPadX);
    const int lx1 = pad + slots_w - S(theme::kMapRowPadX);
    const int listy = S(theme::kLobbyOptionsTop) + S(theme::kMapRowPadY);
    const int page = lobby_rows_per_page(view_h);
    const int first = map_scroll_;
    for (int i = 0; i < page && first + i < int(maps_.size()); ++i) {
        const int y = listy + i * pitch;
        out.push_back({lx0, y, lx1, y + pd_h, maps_[first + i].name, "", kMapRow0 + first + i});
    }
    if (int(maps_.size()) > page) {
        const int sy = listy + page * pitch;
        const int half = (lx1 - lx0 - S(theme::kSlotGapX)) / 2;
        out.push_back({lx0, sy, lx0 + half, sy + pd_h, "Prev", "", kActScrollUp});
        out.push_back({lx1 - half, sy, lx1, sy + pd_h, "Next", "", kActScrollDown});
    }
    if (maps_folder_cb_) {
        const int mi_x1 = W - pad, mi_x0 = mi_x1 - S(theme::kMapInfoW);
        const int mp = S(theme::kMapInfoPad);
        const int by = S(theme::kLobbySlotsTop) + mp + S(theme::kMinimapBox) - S(theme::kActionH);
        out.push_back({mi_x0 + mp + S(theme::kMinimapBox) + mp, by, mi_x1 - mp, by + S(theme::kActionH),
                       "Maps Folder...", "", kActMapsFolder});
    }
    const int bw = S(theme::kPrimaryW), bh = S(theme::kPrimaryH);
    const int by = H - S(theme::kLobbyActionsBottom) - bh;
    const int sx1 = W - pad;
    out.push_back({sx1 - bw, by, sx1, by + bh, "START GAME", "", kActStart});
    out.push_back({sx1 - bw - S(theme::kActionGap) - bw, by, sx1 - bw - S(theme::kActionGap),
                   by + bh, "LEAVE LOBBY", "", kActBack});
    return out;
}

std::vector<FrontEnd::Button> FrontEnd::replay_buttons(std::uint32_t view_w, std::uint32_t view_h) const {
    std::vector<Button> out;
    const int W = int(view_w), H = int(view_h);
    auto S = [view_h](int authored) { return theme::scaled(authored, view_h); };
    const int pad = S(theme::kScreenPad);
    const int row_h = S(theme::kPulldownH) + S(theme::kMapRowPadY) * 2;
    const int listy = S(theme::kFlowStripH) + S(theme::kPagePad) * 2;
    const int lx0 = pad, lx1 = W - pad;
    const int bh = S(theme::kPrimaryH);
    const int by = H - S(theme::kFooterH) - S(theme::kPagePad) - bh;
    const int rows = std::max(0, (by - S(theme::kPagePad) - listy) / (row_h + S(theme::kMapRowGap)));
    for (int i = 0; i < rows && i < int(replays_.size()); ++i) {
        const int y = listy + i * (row_h + S(theme::kMapRowGap));
        out.push_back({lx0, y, lx1, y + row_h, replays_[std::size_t(i)].name, "", kReplayRow0 + i});
    }
    const int bw = S(theme::kPrimaryW);
    if (sel_replay_ >= 0 && sel_replay_ < int(replays_.size()))
        out.push_back({lx1 - bw, by, lx1, by + bh, "WATCH", "Watch replay", kActWatch});
    out.push_back({lx0, by, lx0 + bw, by + bh, "BACK", "BackButton", kActBack});
    return out;
}

void FrontEnd::scan_replays() {
    replays_scanned_ = true;
    replays_.clear();
    if (sel_replay_ >= 0) sel_replay_ = -1;
    if (replays_dir_.empty()) return;
    std::error_code ec;
    if (!std::filesystem::is_directory(replays_dir_, ec)) return;
    for (const auto& entry : std::filesystem::directory_iterator(replays_dir_, ec)) {
        if (ec) break;
        if (!entry.is_regular_file(ec)) continue;
        if (entry.path().extension().string() != ".sc2orders") continue;
        replays_.push_back({entry.path().string(), entry.path().stem().string()});
    }
    std::sort(replays_.begin(), replays_.end(), [](const ReplayEntry& a, const ReplayEntry& b) { return a.name > b.name; });
}

std::string FrontEnd::save_game() {
    if (!skirmish_ || skirmish_->replay_active() || saves_dir_.empty()) return std::string();
    std::error_code ec;
    std::filesystem::create_directories(saves_dir_, ec);
    const auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    char stamp[32];
    std::tm tmv{};
#ifdef _WIN32
    localtime_s(&tmv, &now);
#else
    localtime_r(&now, &tmv);
#endif
    std::strftime(stamp, sizeof stamp, "%Y%m%d-%H%M%S", &tmv);
    std::string path = saves_dir_ + "/" + stamp + ".sc2orders";
    for (int n = 1; std::filesystem::exists(path, ec) && n < 100; ++n) path = saves_dir_ + "/" + stamp + "-" + std::to_string(n) + ".sc2orders";
    skirmish_->set_replay_meta(match_map_path_, {std::int32_t(p0_race_), std::int32_t(p1_race_)});
    return skirmish_->write_replay(path) ? path : std::string();
}

std::string FrontEnd::newest_save() const {
    if (saves_dir_.empty()) return std::string();
    std::error_code ec;
    if (!std::filesystem::is_directory(saves_dir_, ec)) return std::string();
    std::string best;
    for (std::filesystem::directory_iterator it(saves_dir_, ec); !ec && it != std::filesystem::directory_iterator(); it.increment(ec)) {
        if (!it->is_regular_file(ec) || it->path().extension().string() != ".sc2orders") continue;
        const std::string p = it->path().string();
        if (best.empty() || it->path().filename().string() > std::filesystem::path(best).filename().string()) best = p;
    }
    return best;
}

bool FrontEnd::load_save(const std::string& path) {
    OrderLog log;
    if (!read_order_log_file(path, log)) return false;
    if (skirmish_) end_match();
    replay_log_pending_ = std::move(log);
    replay_pending_ = true;
    resume_pending_ = true;
    sel_map_ = 0;
    for (std::size_t i = 0; i < maps_.size(); ++i)
        if (!replay_log_pending_.map_id.empty() && maps_[i].path == replay_log_pending_.map_id) sel_map_ = int(i);
    if (replay_log_pending_.player_races.size() >= 2) {
        p0_race_ = Race(std::max(0, std::min(2, int(replay_log_pending_.player_races[0]))));
        p1_race_ = Race(std::max(0, std::min(2, int(replay_log_pending_.player_races[1]))));
    }
    match_error_.clear();
    loading_frames_ = 0;
    load_plan_.reset();
    if (replay_log_pending_.map_id.empty()) start_skirmish();
    else router_.replace(ScreenId::Loading);
    return true;
}

void FrontEnd::record_match_replay() {
    last_recorded_replay_.clear();
    if (!skirmish_ || skirmish_->replay_active() || replays_dir_.empty()) return;
    std::error_code ec;
    std::filesystem::create_directories(replays_dir_, ec);
    const auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    char stamp[32];
    std::tm tmv{};
#ifdef _WIN32
    localtime_s(&tmv, &now);
#else
    localtime_r(&now, &tmv);
#endif
    std::strftime(stamp, sizeof stamp, "%Y%m%d-%H%M%S", &tmv);
    std::string path = replays_dir_ + "/" + stamp + ".sc2orders";
    for (int n = 1; std::filesystem::exists(path, ec) && n < 100; ++n) path = replays_dir_ + "/" + stamp + "-" + std::to_string(n) + ".sc2orders";
    skirmish_->set_replay_meta(match_map_path_, {std::int32_t(p0_race_), std::int32_t(p1_race_)});
    if (skirmish_->write_replay(path)) last_recorded_replay_ = path;
}

void FrontEnd::start_sandbox() {
    sandbox_ = std::make_unique<SandboxGame>(cat_, models_, store_, map_);
    router_.replace(ScreenId::Sandbox);
}

void FrontEnd::start_skirmish() {
    clear_score();
    p0_race_ = Race::Terran;
    p1_race_ = Race::Terran;
    p0_pick_ = p1_pick_ = LobbyRace::Terran;
    sel_map_ = 0;
    begin_match();
}

void FrontEnd::invalidate_map_if_changed(const std::string& path) {
    if (!api_map_bytes_.empty() || path != built_map_path_) {
        map_built_ = false;
        pending_map_bytes_.clear();
    }
}

void FrontEnd::plan_load() {
    load_plan_.reset();
    load_plan_.planned = true;
    auto add = [&](LoadStep::Kind k, std::string arg, std::string label) {
        load_plan_.steps.push_back(LoadStep{k, std::move(arg), std::move(label)});
    };
    const bool synthetic = api_map_bytes_.empty() &&
                           (sel_map_ <= 0 || sel_map_ >= int(maps_.size()) ||
                            maps_[std::size_t(sel_map_)].path.empty());
    match_map_path_ = synthetic || !api_map_bytes_.empty()
                          ? std::string()
                          : maps_[std::size_t(sel_map_)].path;
    invalidate_map_if_changed(match_map_path_);
    if (!synthetic) {
        add(LoadStep::Kind::MapBytes, {}, "Reading the map archive");
        add(LoadStep::Kind::MapTerrain, {}, "Decoding terrain");
        add(LoadStep::Kind::MapCliffs, {}, "Placing cliffs");
        add(LoadStep::Kind::MapMesh, {}, "Building the terrain mesh");
    }
    add(LoadStep::Kind::Layout, {}, "Loading the interface layout");
    add(LoadStep::Kind::Font, {}, "Loading fonts");
    const Race rs[2] = {p0_race_, p1_race_};
    for (int p = 0; p < 2; ++p) {
        const RaceUnits u = race_units(rs[p]);
        for (const char* tok : {u.hall, u.worker, u.producer, u.supply, u.army})
            if (tok && *tok) add(LoadStep::Kind::Model, tok, std::string("Loading ") + tok);
    }
}

bool FrontEnd::step_load(std::uint32_t view_w, std::uint32_t view_h) {
    if (load_plan_.done >= load_plan_.steps.size()) return false;
    const LoadStep& s = load_plan_.steps[load_plan_.done];
    load_plan_.label = s.label;
    switch (s.kind) {
        case LoadStep::Kind::MapBytes:
        case LoadStep::Kind::MapTerrain:
        case LoadStep::Kind::MapCliffs:
        case LoadStep::Kind::MapMesh: {
            const int phase = int(s.kind) - int(LoadStep::Kind::MapBytes);
            if (!build_real_map_phase(phase)) {
                load_plan_.reset();
                router_.replace(ScreenId::Lobby);
                return false;
            }
            ++load_plan_.warmed;
            break;
        }
        case LoadStep::Kind::Layout: {
            const float aspect = view_h ? float(view_w) / float(view_h) : 16.0f / 9.0f;
            if (hud_authored_rects(store_, aspect)) ++load_plan_.warmed;
            else ++load_plan_.missing;
            break;
        }
        case LoadStep::Kind::Font: {
            ensure_overlay_font();
            if (draw::active_font()) ++load_plan_.warmed;
            else ++load_plan_.missing;
            break;
        }
        case LoadStep::Kind::Model:
        default: {
            if (models_.ensure(store_, s.arg)) ++load_plan_.warmed;
            else ++load_plan_.missing;
            break;
        }
    }
    ++load_plan_.done;
    return true;
}

bool FrontEnd::build_real_map_phase(int phase) {
    if (map_built_) return true;
    try {
        if (phase == 0) {
            if (!api_map_bytes_.empty()) {
                pending_map_bytes_ = std::move(api_map_bytes_);
                api_map_bytes_.clear();
            } else if (maps_[std::size_t(sel_map_)].casc) {
                pending_map_bytes_ = store_.read_by_name(maps_[std::size_t(sel_map_)].path);
            } else {
                std::ifstream in(maps_[std::size_t(sel_map_)].path, std::ios::binary);
                pending_map_bytes_.assign(std::istreambuf_iterator<char>(in),
                                          std::istreambuf_iterator<char>());
            }
        } else if (phase == 1) {
            real_map_ = std::make_unique<formats::LoadedMap>(
                formats::load_map_terrain(std::move(pending_map_bytes_)));
            pending_map_bytes_.clear();
            app::stamp_map_doodads(cat_, *real_map_);
            app::stamp_map_units(cat_, *real_map_);
            current_env_ = data::resolve_environment_light(cat_, real_map_->tileset);
        } else if (phase == 2) {
            real_cliffs_ = app::place_cliffs(store_, *real_map_);
        } else {
            const bool have_cliffs = !real_cliffs_.instances.empty();
            real_ground_ = formats::build_map_mesh_stepped(*real_map_, 1.0f, 128, !have_cliffs);
            if (texture_cb_) texture_cb_(real_ground_, *real_map_);
            const auto water_heights = water_type_heights(store_);
            real_water_ = formats::build_map_water(*real_map_, 1.0f, 0.5f, &water_heights);
            real_water_rgba_ = {1.0f, 1.0f, 1.0f, 1.0f};
            if (!real_map_->water.planes.empty()) {
                const auto mats = water_type_materials(store_);
                const auto it = mats.find(real_map_->water.planes.front().type);
                if (it != mats.end())
                    real_water_rgba_ = {it->second.color[0], it->second.color[1], it->second.color[2], it->second.color[3]};
            }
            map_built_ = true;
            built_map_path_ = match_map_path_;
        }
        return true;
    } catch (const std::exception& e) {
        const std::string which = sel_map_ >= 0 && sel_map_ < int(maps_.size())
                                      ? maps_[std::size_t(sel_map_)].name
                                      : std::string("the supplied archive");
        match_error_ = "cannot load \"" + which + "\": " + e.what();
        real_map_.reset();
        real_cliffs_ = CliffField{};
        real_doodads_.clear();
        api_map_bytes_.clear();
        pending_map_bytes_.clear();
        map_built_ = false;
        return false;
    }
}

bool FrontEnd::begin_match() {
    match_error_.clear();
    const bool synthetic = api_map_bytes_.empty() && (sel_map_ <= 0 || sel_map_ >= int(maps_.size()) ||
                                                      maps_[sel_map_].path.empty());
    match_map_path_ = synthetic || !api_map_bytes_.empty() ? std::string() : maps_[sel_map_].path;
    invalidate_map_if_changed(match_map_path_);
    MeleeSetup setup;
    const formats::Model* ground = nullptr;
    float map_w = 0, map_h = 0;
    const sim::NavGrid* nav = nullptr;
    const sim::TerrainMap* placement = nullptr;
    float playable_x0 = -1.0f, playable_y0 = -1.0f, playable_x1 = -1.0f, playable_y1 = -1.0f;

    if (synthetic) {
        data::Production prod;
        prod.build(cat_);
        const Race rs[2] = {p0_race_, p1_race_};
        for (int p = 0; p < 2; ++p) {
            const MeleeProtos pr = race_protos(cat_, rs[p]);
            const TestMap::Base& base = map_.players[p].main;
            MeleePlayerSetup ps;
            ps.start = {Fx(base.town.x), Fx(base.town.y)};
            ps.town_hall = pr.hall;
            ps.town_hall_facing = pr.hall_facing;
            ps.worker = pr.worker;
            ps.worker_count = pr.worker_count;
            ps.worker_offsets = pr.worker_offsets;
            ps.start_extras = pr.start_extras;
            ps.mineral_field = pr.mineral;
            ps.geyser_field = pr.geyser;
            for (const TestMap::Resource& res : base.resources) {
                if (res.kind == TestMap::ResourceKind::Mineral)
                    ps.mineral_positions.push_back({Fx(res.position.x), Fx(res.position.y)});
                else if (res.kind == TestMap::ResourceKind::Geyser)
                    ps.geyser_positions.push_back({Fx(res.position.x), Fx(res.position.y)});
            }
            ps.producer = pr.producer;
            ps.supply_unit = pr.supply;
            ps.army_unit = pr.army;
            ps.start_minerals = pr.start_minerals;
            ps.food_ceiling = pr.food_ceiling;
            apply_race_costs(ps, cat_, rs[p], prod);
            setup.players.push_back(ps);
        }
        ground = &map_.ground;
        map_w = map_.width;
        map_h = map_.height;
        nav = &map_.nav;
    } else {
        for (int phase = 0; phase < 4; ++phase)
            if (!build_real_map_phase(phase)) {
                router_.replace(ScreenId::Lobby);
                return false;
            }
        setup = melee_setup_for_map(*real_map_, cat_, p0_race_, p1_race_, -1, 8,
                                    replay_pending_ ? replay_log_pending_.seed : MeleeGameConfig{}.seed);
        ground = &real_ground_;
        map_w = float(real_map_->width);
        map_h = float(real_map_->height);
        nav = &real_map_->nav;
        placement = &real_map_->placement;
        if (real_map_->info.has_playable_area()) {
            playable_x0 = float(real_map_->info.playable_x0);
            playable_y0 = float(real_map_->info.playable_y0);
            playable_x1 = float(real_map_->info.playable_x1);
            playable_y1 = float(real_map_->info.playable_y1);
        }
    }

    const Race rs2[2] = {p0_race_, p1_race_};
    for (int p = 0; p < 2; ++p) {
        const RaceUnits u = race_units(rs2[p]);
        for (const char* tok : {u.hall, u.worker, u.producer, u.supply, u.army})
            models_.ensure(store_, tok);
    }
    for (const sim::Unit& n : setup.neutrals)
        if (!n.catalog_id.empty()) models_.ensure(store_, n.catalog_id);

    MeleeGameConfig cfg;
    cfg.human_player = 0;
    if (replay_pending_) cfg.seed = replay_log_pending_.seed;
    skirmish_ = std::make_unique<MeleeGame>(cfg);
    skirmish_->set_ground_fog(ground_fog_);
    if (console_renderer_) skirmish_->set_console_renderer(console_renderer_);
    if (portrait_renderer_) skirmish_->set_portrait_renderer(portrait_renderer_);
    skirmish_->set_ui_storage(&store_);
    skirmish_->set_build_verb(sim::BuildVerb::Committed);
    skirmish_->register_creep_masks(cat_);
    skirmish_->set_creep_diffuse(render::resolve_creep_diffuse(store_));
    if (real_map_) {
        skirmish_->set_tier_heights(formats::tier_heights(*real_map_));
        skirmish_->set_air_floor(formats::air_floor_z(*real_map_));
        std::uint32_t gw = 0, gh = 0;
        std::vector<float> gh_grid = formats::bake_ground_heights(*real_map_, gw, gh);
        if (!gh_grid.empty()) skirmish_->set_ground_heights(std::move(gh_grid), gw, gh);
        skirmish_->set_minimap_image(real_map_->minimap_rgba, real_map_->minimap_w,
                                     real_map_->minimap_h);
    }
    skirmish_->setup(setup, &models_, ground, map_w, map_h, nav, placement, playable_x0,
                     playable_y0, playable_x1, playable_y1);
    if (real_map_) skirmish_->set_water(&real_water_, real_water_rgba_);
    if (real_map_) {
        real_doodads_ = real_map_->objects.doodads;
        const auto& cm = real_map_->cliff;
        const std::vector<float> th = formats::tier_heights(*real_map_);
        for (formats::PlacedDoodad& d : real_doodads_) {
            const int cx = int(d.x), cy = int(d.y);
            if (!th.empty() && !cm.empty() && cx >= 0 && cy >= 0 && std::uint32_t(cx) < cm.width &&
                std::uint32_t(cy) < cm.height) {
                const std::size_t L = std::size_t(cm.levels[std::size_t(cy) * cm.width + cx]);
                d.z = th[L < th.size() ? L : th.size() - 1];
            } else {
                d.z = 0.0f;
            }
        }
        skirmish_->set_doodads(&real_doodads_);
        skirmish_->set_cliffs(&real_cliffs_.instances);
    }
    if (!research_up_) {
        research_up_ = std::make_unique<data::UpgradeCatalog>();
        try {
            research_up_->load_melee(store_);
        } catch (const std::exception&) {
        }
    }
    const std::vector<Race> races = {p0_race_, p1_race_};
    apply_shipping_wiring(*skirmish_, cat_, research_up_.get(), &setup, &races);
    for (int p = 0; p < 2; ++p) {
        const RaceUnits u = race_units(rs2[p]);
        skirmish_->set_spawn_models(p, models_.model_for(u.worker), models_.model_for(u.army),
                                    models_.model_for(u.producer));
    }
    skirmish_->set_pause_actions([this] { save_game(); }, [this] { load_pending_path_ = newest_save(); });
    if (replay_pending_) {
        skirmish_->begin_replay(replay_log_pending_);
        replay_pending_ = false;
        if (resume_pending_) {
            resume_pending_ = false;
            skirmish_->run_replay_to_end();
            skirmish_->resume_live();
            if (router_.top() == ScreenId::Loading) router_.follow("load complete");
            else router_.replace(ScreenId::InGame);
        } else if (router_.top() == ScreenId::Loading) router_.follow("replay load complete");
        else router_.replace(ScreenId::ReplayControls);
    } else if (router_.top() == ScreenId::Loading)
        router_.follow("load complete");
    else
        router_.replace(ScreenId::InGame);
    return true;
}

bool FrontEnd::step(const render::InputState& in, float dt_ms,
                    std::vector<render::SceneInstance>& scene, render::Camera& cam) {
    if (!load_pending_path_.empty()) {
        const std::string path = load_pending_path_;
        load_pending_path_.clear();
        if (!load_save(path)) match_error_ = "cannot read save " + path;
    }
    if (skirmish_) {
        const bool open = skirmish_->pause_menu_open();
        if (open && router_.top() == ScreenId::InGame)
            router_.follow("Esc / F10");
        else if (!open && router_.top() == ScreenId::PauseMenu)
            router_.follow("Resume");
    }
    if (router_.top() == ScreenId::ReplayBrowse) {
        if (!replays_scanned_) scan_replays();
    } else {
        replays_scanned_ = false;
    }
    if (router_.top() == ScreenId::ReplayControls && skirmish_) {
        if (in.quit) {
            release_match();
            router_.follow("Leave replay");
            prev_left_ = in.left_pressed; scene.clear(); cam = menu_ctl_.camera();
            return true;
        }
        skirmish_->step(in, dt_ms, scene, cam);
        if (skirmish_ && (skirmish_->world().tick() >= skirmish_->replay_total_ticks() ||
                          skirmish_->outcome() != MatchResult::Outcome::Ongoing)) {
            last_outcome_ = skirmish_->outcome();
            release_match();
            router_.follow("end of replay");
            scene.clear(); cam = menu_ctl_.camera();
        }
        prev_left_ = in.left_pressed;
        return true;
    }
    if ((router_.top() == ScreenId::InGame || router_.top() == ScreenId::PauseMenu) && skirmish_) {
        if (!skirmish_->step(in, dt_ms, scene, cam)) {
            record_match_replay();
            release_match();
            if (router_.top() == ScreenId::PauseMenu) router_.follow("Quit to Main Menu");
            else router_.unwind_to(ScreenId::GlueRoot);
            prev_left_ = false; scene.clear(); cam = menu_ctl_.camera();
            return true;
        }
        if (router_.top() == ScreenId::InGame &&
            skirmish_->outcome() != MatchResult::Outcome::Ongoing) {
            last_outcome_ = skirmish_->outcome();
            record_match_replay();
            release_match();
            router_.follow("win/lose condition met");
            prev_left_ = in.left_pressed;
            scene.clear();
            cam = menu_ctl_.camera();
        }
        return true;
    }
    if (router_.top() == ScreenId::Sandbox && sandbox_) {
        if (!sandbox_->step(in, dt_ms, scene, cam)) {
            sandbox_.reset(); router_.follow("Leave sandbox"); scene.clear();
            cam = menu_ctl_.camera();
        }
        return true;
    }
    if (router_.top() == ScreenId::Loading) {
        if (in.quit) {
            router_.cancel();
            loading_frames_ = 0;
            load_plan_.reset();
        } else if (loading_frames_ < 1) {
            ++loading_frames_;
        } else {
            if (!load_plan_.planned) plan_load();
            if (!step_load(in.view_w, in.view_h)) {
                if (router_.top() == ScreenId::Loading) {
                    loading_frames_ = 0;
                    begin_match();
                }
            }
        }
        prev_left_ = in.left_pressed;
        scene.clear();
        cam = menu_ctl_.camera();
        return true;
    }

    const bool click = in.left_pressed && !prev_left_ && in.view_w && in.view_h;
    if (router_.top() == ScreenId::ReplayBrowse) {
        if (in.quit) { router_.cancel(); }
        else if (click)
            for (const Button& b : replay_buttons(in.view_w, in.view_h)) {
                if (!b.hit(in.mouse_x, in.mouse_y)) continue;
                if (b.action >= kReplayRow0) sel_replay_ = b.action - kReplayRow0;
                else if (b.action == kActBack) router_.follow("BackButton");
                else if (b.action == kActWatch && sel_replay_ >= 0 && sel_replay_ < int(replays_.size())) {
                    OrderLog log;
                    if (read_order_log_file(replays_[std::size_t(sel_replay_)].path, log)) {
                        replay_log_pending_ = std::move(log);
                        replay_pending_ = true;
                        replay_name_ = replays_[std::size_t(sel_replay_)].name;
                        sel_map_ = 0;
                        for (std::size_t i = 0; i < maps_.size(); ++i)
                            if (!replay_log_pending_.map_id.empty() && maps_[i].path == replay_log_pending_.map_id) sel_map_ = int(i);
                        if (replay_log_pending_.player_races.size() >= 2) {
                            p0_race_ = Race(std::max(0, std::min(2, int(replay_log_pending_.player_races[0]))));
                            p1_race_ = Race(std::max(0, std::min(2, int(replay_log_pending_.player_races[1]))));
                        }
                        match_error_.clear();
                        loading_frames_ = 0;
                        load_plan_.reset();
                        router_.follow("Watch replay");
                    } else {
                        match_error_ = "cannot read replay " + replays_[std::size_t(sel_replay_)].name;
                    }
                }
                break;
            }
        prev_left_ = in.left_pressed;
        scene.clear();
        cam = menu_ctl_.camera();
        return true;
    }
    if (router_.top() != ScreenId::GlueRoot && router_.top() != ScreenId::Lobby) {
        if (in.quit) {
            if (!router_.cancel()) { prev_left_ = in.left_pressed; return false; }
        } else if (click) {
            click_glue_screen(router_.top(), in.mouse_x, in.mouse_y, in.view_w, in.view_h);
        }
        if (router_.top() == ScreenId::Boot) { prev_left_ = in.left_pressed; return false; }
        if (router_.top() == ScreenId::Loading) { loading_frames_ = 0; load_plan_.reset(); }
        prev_left_ = in.left_pressed;
        scene.clear();
        cam = menu_ctl_.camera();
        return true;
    }
    if (router_.top() == ScreenId::GlueRoot) {
        if (click) {
            const std::vector<Button> bs = menu_buttons(in.view_w, in.view_h);
            for (auto it = bs.rbegin(); it != bs.rend(); ++it)
                if (it->hit(in.mouse_x, in.mouse_y)) {
                    router_.follow(trigger_for_control(ScreenId::GlueRoot, it->trigger));
                    break;
                }
        } else if (in.quit)
            router_.cancel();
        if (router_.top() == ScreenId::Sandbox && !sandbox_) start_sandbox();
        if (router_.top() == ScreenId::Boot) { prev_left_ = in.left_pressed; return false; }
    } else if (router_.top() == ScreenId::Lobby) {
        if (in.view_h) {
            if (in.wheel != 0.0f) map_scroll_ -= int(in.wheel > 0.0f ? 1 : -1) * 3;
            clamp_map_scroll(in.view_h);
        }
        if (in.quit) { router_.cancel(); }
        else if (click)
            for (const Button& b : lobby_buttons(in.view_w, in.view_h)) {
                if (!b.hit(in.mouse_x, in.mouse_y)) continue;
                if (b.action >= kMapRow0) { sel_map_ = b.action - kMapRow0; match_error_.clear(); }
                else if (b.action == kActP0Race) {
                    p0_pick_ = lobby_race_cycle(p0_pick_);
                    if (p0_pick_ != LobbyRace::Random) p0_race_ = Race(int(p0_pick_));
                } else if (b.action == kActP1Race) {
                    p1_pick_ = lobby_race_cycle(p1_pick_);
                    if (p1_pick_ != LobbyRace::Random) p1_race_ = Race(int(p1_pick_));
                }
                else if (b.action == kActMapsFolder) { if (maps_folder_cb_) maps_folder_cb_(); }
                else if (b.action == kActScrollUp) { map_scroll_ -= lobby_rows_per_page(in.view_h); clamp_map_scroll(in.view_h); }
                else if (b.action == kActScrollDown) { map_scroll_ += lobby_rows_per_page(in.view_h); clamp_map_scroll(in.view_h); }
                else if (b.action == kActBack) router_.follow("Back / leave lobby");
                else if (b.action == kActStart) {
                    const std::uint64_t match_seed = MeleeGameConfig{}.seed;
                    p0_race_ = resolve_lobby_race(p0_pick_, match_seed, 0);
                    p1_race_ = resolve_lobby_race(p1_pick_, match_seed, 1);
                    match_error_.clear();
                    loading_frames_ = 0;
                    load_plan_.reset();
                    router_.follow("StartButton -> countdown");
                }
                break;
            }
    }
    prev_left_ = in.left_pressed;

    scene.clear();
    cam = menu_ctl_.camera();
    return true;
}

void FrontEnd::ensure_overlay_font() {
    if (overlay_font_tried_) {
        if (overlay_font_.valid()) draw::set_active_font(&overlay_font_);
        return;
    }
    overlay_font_tried_ = true;
    static const char* kMods[] = {"mods/core.sc2mod/base.sc2data/", "mods/liberty.sc2mod/base.sc2data/",
                                  "mods/swarm.sc2mod/base.sc2data/", "mods/void.sc2mod/base.sc2data/"};
    ui::StyleSet styles;
    for (const char* mod : kMods) {
        try {
            const auto bytes = store_.read_by_name(std::string(mod) + "UI/FontStyles.SC2Style");
            styles.ingest(std::string(bytes.begin(), bytes.end()));
        } catch (const std::exception&) {   }
    }
    for (const char* sn : {"GlueTitle", "GlueTitleSmall", "GlueDialogTitle", "GlueLabel", "GlueValue",
                           "GlueButton", "GlueButtonSmall", "GlueDescription"}) {
        const ui::FontStyle st = styles.resolve(sn);
        if (st.has_color) style_rgb_[sn] = theme::Rgb{st.r, st.g, st.b};
    }
    auto load_face = [&](const char* style_name, render::OverlayFont& into, std::string& path_out) {
        const ui::FontStyle st = styles.resolve(style_name);
        if (st.font.empty()) return false;
        for (const char* mod : kMods) {
            const std::string path = std::string(mod) + st.font;
            try {
                const auto bytes = store_.read_by_name(path);
                if (into.load(bytes)) { path_out = path; return true; }
            } catch (const std::exception&) {   }
        }
        return false;
    };
    if (!load_face("GlueButton", overlay_font_, face_path_[0])) {
        static const char* kCandidates[] = {
            "mods/core.sc2mod/base.sc2data/UI/Fonts/Eurostile-Reg.otf",
            "mods/core.sc2mod/base.sc2data/UI/Fonts/Eurostile-Bol.otf",
            "mods/core.sc2mod/base.sc2data/UI/Fonts/BLQ55.ttf",
            "mods/core.sc2mod/base.sc2data/UI/Fonts/BLQ85.ttf",
            "mods/core.sc2mod/base.sc2data/UI/Fonts/bl.ttf",
        };
        for (const char* path : kCandidates) {
            try {
                auto bytes = store_.read_by_name(path);
                if (overlay_font_.load(bytes)) break;
            } catch (const std::exception&) {   }
        }
    }
    if (!load_face("GlueTitle", overlay_font_ext_, face_path_[1]) && overlay_font_.valid()) {
        try {
            const auto bytes = store_.read_by_name(face_path_[0].empty() ? "mods/core.sc2mod/base.sc2data/UI/Fonts/Eurostile-Reg.otf" : face_path_[0]);
            overlay_font_ext_.load(bytes);
        } catch (const std::exception&) {
        }
    }
    if (overlay_font_.valid()) draw::set_active_font(&overlay_font_);
}

std::vector<std::pair<std::string, ui::Rect>> FrontEnd::screen_controls(std::uint32_t view_w,
                                                                       std::uint32_t view_h) {
    std::vector<std::pair<std::string, ui::Rect>> out;
    const ScreenId id = router_.top();
    auto push_buttons = [&out](const std::vector<Button>& bs) {
        for (const Button& b : bs)
            out.push_back({b.trigger.empty() ? b.label : b.trigger,
                           ui::Rect{float(b.x0), float(b.y0), float(b.x1 - b.x0),
                                    float(b.y1 - b.y0)}});
    };
    if (id == ScreenId::GlueRoot) {
        push_buttons(menu_buttons(view_w, view_h));
        return out;
    }
    if (id == ScreenId::Lobby) {
        push_buttons(lobby_buttons(view_w, view_h));
        return out;
    }
    if (id == ScreenId::ReplayBrowse) {
        push_buttons(replay_buttons(view_w, view_h));
        return out;
    }
    if (const GlueScreen* s = glue_screen(id, view_w, view_h))
        for (std::size_t i = 0; i < s->controls().size(); ++i)
            out.push_back({s->controls()[i].name, s->hit_rect(i)});
    return out;
}

bool FrontEnd::current_screen_is_authored(std::uint32_t view_w, std::uint32_t view_h) {
    const ScreenId id = router_.top();
    if (id == ScreenId::GlueRoot) return real_menu(view_w, view_h) != nullptr;
    if (id == ScreenId::Lobby) return false;
    const GlueScreen* s = glue_screen(id, view_w, view_h);
    return s && !s->controls().empty();
}

void FrontEnd::draw_glue_screen(ScreenId id, std::uint32_t view_w, std::uint32_t view_h,
                                std::vector<std::uint8_t>& out) {
    Board b{out, view_w, view_h};
    b.face[0] = &overlay_font_;
    b.face[1] = &overlay_font_ext_;
    b.style_rgb = &style_rgb_;
    band_flow_strip(b, screen_name(id));

    if ((id == ScreenId::GameResult || id == ScreenId::Score) &&
        last_outcome_ != MatchResult::Outcome::Ongoing) {
        const bool won = last_outcome_ == MatchResult::Outcome::Victory;
        const int ty = b.s(theme::kLoadTitleTop);
        text_box(b, 0, ty, int(view_w), ty + draw::text_h_em(type_px(b, theme::kTypeLoadTitle)),
                 MatchResult::banner(last_outcome_), theme::kTypeLoadTitle,
                 won ? theme::kOk : theme::kErr);
    }

    const GlueScreen* s = glue_screen(id, view_w, view_h);
    const std::vector<const char*>& docs = screen_layouts(id);
    const int pad = b.s(theme::kScreenPad);
    const int y = b.s(theme::kFlowStripH) + b.s(theme::kPagePad);
    if (!s || s->controls().empty()) {
        const std::string why =
            docs.empty()
                ? std::string("this screen has no authored layout document")
                : (!s ? std::string("this install did not supply ") + docs[0] + " and " +
                            std::to_string(docs.size() - 1) + " more"
                      : std::string("read ") + std::to_string(s->documents()) + " of " +
                            std::to_string(docs.size()) + " documents; the solver placed no "
                                                          "clickable control");
        text_at(b, pad, y, why, theme::kTypeBody, theme::kWarn, int(view_w) - pad);
        text_at(b, pad, y + b.s(theme::kRowH), "nothing is drawn here rather than a made-up panel",
                theme::kTypeMicro, theme::kTextMuted, int(view_w) - pad);
        band_footer(b);
        return;
    }
    const std::string src = std::to_string(s->controls().size()) + " authored controls from " +
                            std::to_string(s->documents()) + " of " + std::to_string(docs.size()) +
                            " documents";
    text_at(b, pad, y, src, theme::kTypeCaption, theme::kTextFaint, int(view_w) - pad);
    for (std::size_t i = 0; i < s->controls().size(); ++i) {
        const ui::SolvedFrame& f = s->controls()[i];
        const ui::Rect& r = s->hit_rect(i);
        const int x0 = int(r.x), y0 = int(r.y);
        const int x1 = int(r.x + r.w), y1 = int(r.y + r.h);
        if (x1 <= x0 || y1 <= y0) continue;
        button_menu(b, x0, y0, x1, y1, f.text.empty() ? f.name : f.text, false,
                    router_.edge_for(trigger_for_control(id, f.name)) != nullptr);
    }
    band_footer(b);
}

void FrontEnd::build_overlay(std::uint32_t view_w, std::uint32_t view_h,
                             std::vector<std::uint8_t>& out) {
    ensure_overlay_font();
    const ScreenId screen = router_.top();
    if (screen == ScreenId::ReplayControls && skirmish_) {
        skirmish_->build_overlay(view_w, view_h, out);
        Board b{out, view_w, view_h};
        b.face[0] = &overlay_font_;
        b.face[1] = &overlay_font_ext_;
    b.style_rgb = &style_rgb_;
        const std::string cap = "REPLAY  " + replay_name_ + "  " + std::to_string(skirmish_->world().tick()) + " / " +
                                std::to_string(skirmish_->replay_total_ticks()) + "   Esc leaves";
        text_at(b, b.s(theme::kScreenPad), b.s(theme::kPagePad), cap, theme::kTypeScreenTitle, theme::kTextHi);
        return;
    }
    if ((screen == ScreenId::InGame || screen == ScreenId::PauseMenu) && skirmish_) {
        skirmish_->build_overlay(view_w, view_h, out);
        return;
    }
    if (screen == ScreenId::Sandbox && sandbox_) { sandbox_->build_overlay(view_w, view_h, out); return; }

    out.assign(std::size_t(view_w) * view_h * 4, 0);
    if (!view_w || !view_h) return;
    Board b{out, view_w, view_h};
    b.face[0] = &overlay_font_;
    b.face[1] = &overlay_font_ext_;
    b.style_rgb = &style_rgb_;

    switch (screen) {
        case ScreenId::GlueRoot:
        case ScreenId::Home:
            background(b, theme::kVignetteCore, theme::kGround);
            break;
        case ScreenId::Loading:
            background(b, theme::kVignetteLoad, theme::kGround);
            break;
        case ScreenId::PauseMenu:
            background(b, theme::kVignettePause, theme::kGroundDeep);
            break;
        default:
            background(b, theme::kVignetteLobby, theme::kGround);
            break;
    }

    std::vector<Ctl> ctls;
    for (const std::pair<std::string, ui::Rect>& c : screen_controls(view_w, view_h)) {
        Ctl k;
        k.x0 = int(c.second.x);
        k.y0 = int(c.second.y);
        k.x1 = int(c.second.x + c.second.w);
        k.y1 = int(c.second.y + c.second.h);
        k.label = c.first;
        k.live = router_.edge_for(trigger_for_control(screen, c.first)) != nullptr;
        if (screen == ScreenId::Options && k.label.size() > 8 && k.label.compare(k.label.size() - 8, 8, "CheckBox") == 0) {
            k.live = true;
            k.selected = option_checked(k.label);
        }
        ctls.push_back(std::move(k));
    }
    if (screen == ScreenId::ReplayBrowse) {
        const std::vector<Button> bs = replay_buttons(view_w, view_h);
        for (std::size_t i = 0; i < ctls.size() && i < bs.size(); ++i) ctls[i].action = bs[i].action;
    }

    if (screen == ScreenId::GlueRoot) {
        const std::vector<Button> bs = menu_buttons(view_w, view_h);
        for (std::size_t i = 0; i < ctls.size() && i < bs.size(); ++i) ctls[i].label = bs[i].label;
        draw_shell_root(b, ctls);
        return;
    }
    if (screen == ScreenId::Home) { draw_home(b, ctls); return; }
    if (screen == ScreenId::Custom) { draw_custom(b, ctls); return; }
    if (screen == ScreenId::ReplayBrowse) {
        band_flow_strip(b, "REPLAYS");
        for (Ctl& c : ctls) c.selected = c.action >= kReplayRow0 && c.action - kReplayRow0 == sel_replay_;
        for (const Ctl& c : ctls) {
            if (c.action == kActWatch) button_primary(b, c.x0, c.y0, c.x1, c.y1, "WATCH", theme::kTypePrimaryLg);
            else if (c.action == kActBack) button_secondary(b, c.x0, c.y0, c.x1, c.y1, "BACK", theme::kTypeSecondaryLg);
            else if (c.action >= kReplayRow0) {
                panel(b, c.x0, c.y0, c.x1, c.y1, c.selected ? theme::kSurfaceSel : theme::kSurfaceRow, theme::kAlphaPanelDim,
                      c.selected ? theme::kAccent : theme::kBorderFaint);
                text_left(b, c.x0 + b.s(theme::kMapRowPadX), c.y0, c.y1, c.label, theme::kTypeSmall, c.selected ? theme::kTextHi : theme::kTextCell,
                          c.x1 - b.s(theme::kMapRowPadX));
            }
        }
        if (replays_.empty())
            text_at(b, b.s(theme::kScreenPad), b.s(theme::kFlowStripH) + b.s(theme::kPagePad) * 2, replays_dir_.empty() ? "no replays folder" : "no replays in " + replays_dir_,
                    theme::kTypeBody, theme::kTextMuted);
        if (!match_error_.empty())
            text_at(b, b.s(theme::kScreenPad), b.by(theme::kFooterH + theme::kPagePad), match_error_, theme::kTypeCaption, theme::kErr);
        band_footer(b);
        return;
    }
    if (screen == ScreenId::Options) {
        draw_options(b, ctls, int(screen_layouts(ScreenId::Options).size()), options_panel_ < 6 ? options_panel_ : options_panel_ + 1);
        for (const Ctl& c : ctls) {
            if (!c.selected || c.label.size() <= 8 || c.label.compare(c.label.size() - 8, 8, "CheckBox") != 0) continue;
            const int inset = std::max(2, (c.x1 - c.x0) / 5);
            draw::fill_a(b.out, b.w, b.h, c.x0 + inset, c.y0 + inset, c.x1 - inset, c.y1 - inset,
                         theme::kAccent.r, theme::kAccent.g, theme::kAccent.b, 255);
        }
        return;
    }
    if (screen == ScreenId::PauseMenu) { draw_pause(b, ctls); return; }
    if (screen == ScreenId::Loading) {
        LoadInfo info;
        info.map_name = sel_map_ >= 0 && sel_map_ < int(maps_.size()) ? maps_[sel_map_].name
                                                                     : std::string("-");
        info.p0_race = race_name(p0_race_);
        info.p1_race = race_name(p1_race_);
        const std::vector<data::TeamColor> tc = data::team_colors(cat_);
        auto to_rgb = [](const float v[4]) {
            auto ch = [](float f) {
                const int n = int(f * 255.0f + 0.5f);
                return std::uint8_t(n < 0 ? 0 : (n > 255 ? 255 : n));
            };
            return theme::Rgb{ch(v[0]), ch(v[1]), ch(v[2])};
        };
        if (tc.size() >= 2) {
            info.p0_color = to_rgb(tc[0].diffuse);
            info.p1_color = to_rgb(tc[1].diffuse);
            info.have_colors = true;
        }
        info.done = int(load_plan_.done);
        info.total = int(load_plan_.steps.size());
        info.stage = load_plan_.label;
        draw_loading(b, info);
        return;
    }
    if (screen == ScreenId::Lobby) {
        const std::vector<Button> bs = lobby_buttons(view_w, view_h);
        for (std::size_t i = 0; i < ctls.size() && i < bs.size(); ++i) {
            ctls[i].label = bs[i].label;
            ctls[i].action = bs[i].action;
            ctls[i].selected = bs[i].action >= kMapRow0 && (bs[i].action - kMapRow0) == sel_map_;
        }
        LobbyInfo info;
        info.selected = sel_map_ >= 0 && sel_map_ < int(maps_.size()) ? maps_[sel_map_].name
                                                                     : std::string("-");
        const int page = lobby_rows_per_page(view_h);
        const int last = std::min(int(maps_.size()), map_scroll_ + page);
        info.count_line = int(maps_.size()) > 1
                              ? std::to_string(maps_.size() - 1) + " maps, showing " +
                                    std::to_string(map_scroll_ + 1) + "-" + std::to_string(last)
                              : std::string("no .SC2Map found - the built-in arena only");
        info.error = match_error_;
        draw_lobby(b, ctls, info, kActStart, kActBack, kActMapsFolder, kMapRow0);
        return;
    }
    draw_glue_screen(screen, view_w, view_h, out);
    if (screen == ScreenId::Score) draw_score_table(view_w, view_h, out);
}

}
