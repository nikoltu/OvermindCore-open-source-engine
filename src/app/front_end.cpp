#include <cstdio>
#include "sc2/app/front_end.hpp"
#include "sc2/render/frame_prof.hpp"
#include "sc2/ui/layout.hpp"
#include "sc2/app/shell_emblem.hpp"
#include "sc2/app/ui_canvas.hpp"
#include "sc2/app/ui_theme.hpp"

#include <algorithm>
#include <cmath>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <unordered_map>
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
#include "sc2/app/shoreline_place.hpp"
#include "sc2/app/water_heights.hpp"
#include "sc2/app/road_place.hpp"
#include "sc2/app/splat_place.hpp"
#include "sc2/app/terrain_doodad_place.hpp"
#include "sc2/app/adjust_terrain_place.hpp"
#include "sc2/app/water_material.hpp"
#include "sc2/casc/storage.hpp"
#include "sc2/core/version.hpp"
#include "sc2/data/actor_model.hpp"
#include "sc2/data/catalog.hpp"
#include "sc2/data/environment.hpp"
#include "sc2/render/creep_layer.hpp"
#include "sc2/data/economy.hpp"
#include "sc2/data/production.hpp"
#include "sc2/app/ui_settings.hpp"
#include "sc2/formats/mpq.hpp"
#include "sc2/formats/tga.hpp"

namespace sc2::app {

static std::uint64_t new_match_seed() {
    if (std::getenv("SC2_FIXED_MATCH_SEED")) return 0;
    const std::uint64_t t = std::uint64_t(std::chrono::steady_clock::now().time_since_epoch().count()) ^
                            (std::uint64_t(std::chrono::system_clock::now().time_since_epoch().count()) << 17);
    return (t * 0x9E3779B97F4A7C15ull) | 1ull;
}

using sim::Fixed;
using sim::Unit;

namespace {
Fixed Fx(float v) { return Fixed::from_ratio(int(std::lround(double(v) * 64.0)), 64); }

bool shell_v1() {
    static const bool v1 = std::getenv("SC2_SHELL_V1") != nullptr;
    return v1;
}
std::string& boot_screen_request() {
    static std::string name;
    return name;
}

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
constexpr int kActAiDiff = 12, kActSpeed = 13, kActQuit = 30, kActQuitYes = 31, kActQuitNo = 32,
              kActProfileName = 40, kActProfileRace = 41, kActOptRail0 = 200, kActOptEngine = 250,
              kActOptRow0 = 300;
constexpr const char* kRailNames[] = {"GAMEPLAY", "GRAPHICS", "VIDEO",    "SOUND",  "VOICE",
                                      "INPUT",    "HOTKEYS",  "COLOR",    "LANGUAGE", "OBSERVER",
                                      "SOCIAL",   "STEREOSCOPIC", "ENGINE"};
constexpr int kRailCount = int(sizeof(kRailNames) / sizeof(kRailNames[0]));
constexpr int kRailHotkeys = 6, kRailEngine = kRailCount - 1;

struct EngineRow {
    enum class Kind { Slider, Pulldown, Toggle };
    const char* key;
    const char* label;
    Kind kind;
};
int label_w(std::uint32_t view_h, const std::string& s, theme::Type t) {
    return draw::text_w_em(s, std::max(1, theme::scaled(t.px, view_h)), t.track);
}

const std::vector<EngineRow>& engine_rows() {
    static const std::vector<EngineRow> k = {
        {"opt/sound.master", "Master Volume", EngineRow::Kind::Slider},
        {"opt/sound.music", "Music", EngineRow::Kind::Slider},
        {"opt/sound.effects", "Sound Effects", EngineRow::Kind::Slider},
        {"opt/sound.voice", "Voices", EngineRow::Kind::Slider},
        {"opt/sound.ui", "Interface Sounds", EngineRow::Kind::Slider},
        {"opt/sound.ambient", "Ambient Sounds", EngineRow::Kind::Slider},
        {"opt/gameplay.unit_status", "Show Unit Life Bars", EngineRow::Kind::Pulldown},
        {"opt/gameplay.game_speed", "Game Speed", EngineRow::Kind::Pulldown},
        {"opt/hotkey.profile", "Hotkey Profile", EngineRow::Kind::Pulldown},
        {"opt/display.mode", "Display Mode", EngineRow::Kind::Pulldown},
        {"opt/display.vsync", "Vertical Sync", EngineRow::Kind::Toggle},
        {"opt/display.ui_scale", "UI Scale", EngineRow::Kind::Slider},
    };
    return k;
}

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
    const render::OverlayFont* face[theme::kFaceCount] = {nullptr, nullptr, nullptr};
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

struct Look {
    float hover = 0.0f;
    bool pressed = false;
    bool focused = false;
    float pulse = 0.0f;
};

theme::Rgb mix_rgb(theme::Rgb a, theme::Rgb c, float t) {
    t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
    auto ch = [t](std::uint8_t x, std::uint8_t y) { return std::uint8_t(int(x) + int((int(y) - int(x)) * t)); };
    return theme::Rgb{ch(a.r, c.r), ch(a.g, c.g), ch(a.b, c.b)};
}

void apply_look(const Board& b, int x0, int y0, int x1, int y1, const Look& k) {
    if (k.pressed) draw::fill_a(b.out, b.w, b.h, x0 + 1, y0 + 1, x1 - 1, y1 - 1, 0, 0, 0, 70);
    if (k.hover > 0.0f) {
        const std::uint8_t a = std::uint8_t(k.hover * 255.0f);
        draw::box_a(b.out, b.w, b.h, x0, y0, x1, y1, theme::kAccentHot.r, theme::kAccentHot.g,
                    theme::kAccentHot.b, a);
        draw::inset_hi(b.out, b.w, b.h, x0, y0, x1, y1, theme::kInsetHi.r, theme::kInsetHi.g,
                       theme::kInsetHi.b, std::uint8_t(k.hover * 90.0f));
    }
    if (k.focused) {
        const std::uint8_t a = std::uint8_t(150.0f + 105.0f * k.pulse);
        draw::box_a(b.out, b.w, b.h, x0 - 2, y0 - 2, x1 + 2, y1 + 2, theme::kAccent.r, theme::kAccent.g,
                    theme::kAccent.b, a);
    }
}

void button_menu(const Board& b, int x0, int y0, int x1, int y1, const std::string& label,
                 bool active, bool live = true, const Look& k = Look{}) {
    if (x1 <= x0 || y1 <= y0) return;
    draw::vgrad(b.out, b.w, b.h, x0, y0, x1, y1, theme::kMenuBtnTop.r, theme::kMenuBtnTop.g,
                theme::kMenuBtnTop.b, theme::kMenuBtnBottom.r, theme::kMenuBtnBottom.g,
                theme::kMenuBtnBottom.b, 255);
    const theme::Rgb border = active ? theme::kAccent : theme::kBorderCtl;
    draw::box_a(b.out, b.w, b.h, x0, y0, x1, y1, border.r, border.g, border.b, 255);
    draw::inset_hi(b.out, b.w, b.h, x0, y0, x1, y1, theme::kInsetHi.r, theme::kInsetHi.g,
                   theme::kInsetHi.b, theme::kAlphaInsetBtn);
    if (live) apply_look(b, x0, y0, x1, y1, k);
    const theme::Rgb label_c = active || k.hover > 0.5f ? theme::kTextHi : theme::kTextBody;
    if (live) {
        text_box(b, x0, y0, x1, y1, label, theme::kTypeMenuBtn, label_c);
    } else {
        text_box(b, x0, y0, x1, y1, label, theme::kTypeMenuBtn, theme::kTextMuted2);
    }
}

void button_primary(const Board& b, int x0, int y0, int x1, int y1, const std::string& label,
                    theme::Type t, const Look& k = Look{}, bool live = true) {
    if (x1 <= x0 || y1 <= y0) return;
    draw::vgrad(b.out, b.w, b.h, x0, y0, x1, y1, theme::kPrimaryTop.r, theme::kPrimaryTop.g,
                theme::kPrimaryTop.b, theme::kPrimaryBottom.r, theme::kPrimaryBottom.g,
                theme::kPrimaryBottom.b, 255);
    draw::box_a(b.out, b.w, b.h, x0, y0, x1, y1, theme::kBorderAccent.r, theme::kBorderAccent.g,
                theme::kBorderAccent.b, 255);
    draw::inset_hi(b.out, b.w, b.h, x0, y0, x1, y1, theme::kInsetPrimary.r, theme::kInsetPrimary.g,
                   theme::kInsetPrimary.b, theme::kAlphaInsetPrimary);
    if (live) apply_look(b, x0, y0, x1, y1, k);
    else draw::fill_a(b.out, b.w, b.h, x0, y0, x1, y1, theme::kGround.r, theme::kGround.g, theme::kGround.b,
                      std::uint8_t(255.0f * (1.0f - theme::kDisabledOpacity)));
    text_box(b, x0, y0, x1, y1, label, t, live ? theme::kTextOnPrimary : theme::kTextMuted2);
}

void button_secondary(const Board& b, int x0, int y0, int x1, int y1, const std::string& label,
                      theme::Type t, const Look& k = Look{}) {
    if (x1 <= x0 || y1 <= y0) return;
    panel(b, x0, y0, x1, y1, theme::kSurfaceRaised, 255, theme::kAccentDeep);
    apply_look(b, x0, y0, x1, y1, k);
    text_box(b, x0, y0, x1, y1, label, t, k.hover > 0.5f ? theme::kTextHi : theme::kTextBody);
}

void pulldown(const Board& b, int x0, int y0, int x1, int y1, const std::string& label,
              theme::Rgb label_c, const Look& k = Look{}, const std::string& caption = std::string()) {
    if (x1 <= x0 || y1 <= y0) return;
    panel(b, x0, y0, x1, y1, theme::kSurfaceCtl, 255, theme::kBorder);
    apply_look(b, x0, y0, x1, y1, k);
    const int pad = b.s(theme::kPulldownPadX);
    const int caret_w = run_w(b, "v", theme::kTypeFine);
    text_left(b, x0 + pad, y0, y1, label, theme::kTypeSmall, k.hover > 0.5f ? theme::kTextHi : label_c,
              x1 - pad - caret_w);
    text_left(b, x1 - pad - caret_w, y0, y1, "v", theme::kTypeFine, theme::kAccentDeep, x1);
    if (!caption.empty()) {
        const int ch = draw::text_h_em(type_px(b, theme::kTypeCaption));
        text_at(b, x0, y0 - ch - b.s(3), caption, theme::kTypeCaption, theme::kTextFaint, x1);
    }
}

void slider(const Board& b, int x0, int y0, int x1, int y1, const std::string& label, float frac,
            const std::string& value_text, const Look& k = Look{}) {
    if (x1 <= x0 || y1 <= y0) return;
    if (k.focused || k.hover > 0.0f)
        draw::fill_a(b.out, b.w, b.h, x0, y0, x1, y1, theme::kSurfaceSel.r, theme::kSurfaceSel.g,
                     theme::kSurfaceSel.b, std::uint8_t(60 + int(60.0f * k.hover)));
    if (k.focused) apply_look(b, x0, y0, x1, y1, Look{0.0f, false, true, k.pulse});
    text_left(b, x0 + b.s(theme::kPulldownPadX), y0, y1, label, theme::kTypeBody, theme::kTextBody,
              x0 + (x1 - x0) / 2);
    const int vw = run_w(b, "100 %", theme::kTypeSmall) + b.s(theme::kPulldownPadX);
    const int tx0 = x0 + (x1 - x0) / 2, tx1 = x1 - vw - b.s(theme::kPulldownPadX);
    const int th = std::max(2, b.s(6)), knob = std::max(4, b.s(14));
    const int ty = (y0 + y1) / 2 - th / 2;
    draw::fill_a(b.out, b.w, b.h, tx0, ty, tx1, ty + th, theme::kSurfaceHi.r, theme::kSurfaceHi.g,
                 theme::kSurfaceHi.b, 255);
    frac = frac < 0.0f ? 0.0f : (frac > 1.0f ? 1.0f : frac);
    const int kx = tx0 + int(float(tx1 - tx0 - knob) * frac) + knob / 2;
    draw::fill_a(b.out, b.w, b.h, tx0, ty, kx, ty + th, theme::kAccent.r, theme::kAccent.g, theme::kAccent.b, 255);
    const theme::Rgb kc = mix_rgb(theme::kBorderCtl, theme::kAccentHot, k.hover);
    draw::fill_a(b.out, b.w, b.h, kx - knob / 2, (y0 + y1) / 2 - knob / 2, kx + knob / 2,
                 (y0 + y1) / 2 + knob / 2, kc.r, kc.g, kc.b, 255);
    draw::box_a(b.out, b.w, b.h, kx - knob / 2, (y0 + y1) / 2 - knob / 2, kx + knob / 2,
                (y0 + y1) / 2 + knob / 2, theme::kTextHi.r, theme::kTextHi.g, theme::kTextHi.b, 160);
    text_left(b, tx1 + b.s(theme::kPulldownPadX), y0, y1, value_text, theme::kTypeSmall, theme::kTextCell, x1);
}

void card(const Board& b, int x0, int y0, int x1, int y1, const std::string& title,
          const std::string& sub, const std::string& go, const Look& k = Look{}) {
    if (x1 <= x0 || y1 <= y0) return;
    const theme::Rgb fill = mix_rgb(theme::kSurface, theme::kSurfaceSel, k.hover);
    panel(b, x0, y0, x1, y1, fill, theme::kAlphaPanel, mix_rgb(theme::kBorder, theme::kAccentHot, k.hover));
    if (k.pressed) draw::fill_a(b.out, b.w, b.h, x0 + 1, y0 + 1, x1 - 1, y1 - 1, 0, 0, 0, 70);
    if (k.focused) apply_look(b, x0, y0, x1, y1, Look{0.0f, false, true, k.pulse});
    const int px = b.s(theme::kTilePadX), py = b.s(theme::kTilePadY);
    int ty = y0 + py;
    text_at(b, x0 + px, ty, title, theme::kTypeTileTitle, theme::kTextHi, x1 - px);
    ty += draw::text_h_em(type_px(b, theme::kTypeTileTitle)) + b.s(6);
    const int rule_w = int(float(x1 - x0 - 2 * px) * (0.35f + 0.65f * k.hover));
    draw::fill_a(b.out, b.w, b.h, x0 + px, ty, x0 + px + rule_w, ty + b.s(theme::kTabRuleH), theme::kAccentPure.r,
                 theme::kAccentPure.g, theme::kAccentPure.b, 255);
    ty += b.s(theme::kTabRuleH) + b.s(10);
    text_at(b, x0 + px, ty, sub, theme::kTypeMicro, theme::kTextMuted, x1 - px);
    const int gh = draw::text_h_em(type_px(b, theme::kTypeGo));
    text_at(b, x0 + px, y1 - py - gh, go, theme::kTypeGo, theme::kAccentDeep, x1 - px);
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
    std::string key;
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    int action = 0;
    bool live = false;
    bool selected = false;
    Look look;
};

void compose_controls(const Board& b, const std::vector<Ctl>& ctls) {
    for (const Ctl& c : ctls) button_menu(b, c.x0, c.y0, c.x1, c.y1, c.label, false, c.live, c.look);
}

void draw_toasts(const Board& b, const std::vector<ShellFx::Toast>& toasts) {
    if (toasts.empty()) return;
    const int th = draw::text_h_em(type_px(b, theme::kTypeBody)) + b.s(theme::kPagePad);
    int y1 = b.by(theme::kFooterH) - b.s(theme::kPagePad);
    for (std::size_t i = toasts.size(); i-- > 0;) {
        const ShellFx::Toast& t = toasts[i];
        const float fade = t.ttl_ms < 400.0f ? t.ttl_ms / 400.0f : 1.0f;
        const int tw = run_w(b, t.text, theme::kTypeBody) + b.s(theme::kPagePad) * 2;
        const int x0 = (int(b.w) - tw) / 2, y0 = y1 - th;
        draw::fill_a(b.out, b.w, b.h, x0, y0, x0 + tw, y1, theme::kSurfaceRaised.r, theme::kSurfaceRaised.g,
                     theme::kSurfaceRaised.b, std::uint8_t(float(theme::kAlphaBand) * fade));
        const theme::Rgb edge = t.error ? theme::kErr : theme::kBorderStrong;
        draw::box_a(b.out, b.w, b.h, x0, y0, x0 + tw, y1, edge.r, edge.g, edge.b, std::uint8_t(255.0f * fade));
        draw::fill_a(b.out, b.w, b.h, x0, y0, x0 + b.s(theme::kVersionRimW), y1, edge.r, edge.g, edge.b,
                     std::uint8_t(255.0f * fade));
        text_box(b, x0, y0, x0 + tw, y1, t.text, theme::kTypeBody, t.error ? theme::kErr : theme::kTextBody);
        y1 = y0 - b.s(theme::kMapRowGap);
    }
}

void draw_tooltip(const Board& b, int mx, int my, const std::string& text) {
    if (text.empty()) return;
    std::vector<std::string> lines;
    std::size_t start = 0;
    while (start <= text.size()) {
        std::size_t nl = text.find('\n', start);
        if (nl == std::string::npos) nl = text.size();
        lines.push_back(text.substr(start, nl - start));
        start = nl + 1;
    }
    const int lh = draw::text_h_em(type_px(b, theme::kTypeSmall)) + b.s(4);
    int tw = 0;
    for (const std::string& l : lines) tw = std::max(tw, run_w(b, l, theme::kTypeSmall));
    const int pad = b.s(theme::kPulldownPadX);
    const int w = tw + pad * 2, h = int(lines.size()) * lh + pad * 2 - b.s(4);
    int x0 = mx + b.s(16), y0 = my + b.s(20);
    if (x0 + w > int(b.w) - b.s(4)) x0 = int(b.w) - b.s(4) - w;
    if (y0 + h > int(b.h) - b.s(4)) y0 = my - b.s(12) - h;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    panel(b, x0, y0, x0 + w, y0 + h, theme::kSurface, 245, theme::kBorderStrong);
    int y = y0 + pad;
    for (const std::string& l : lines) {
        text_at(b, x0 + pad, y, l, theme::kTypeSmall, theme::kTextBody, x0 + w - pad);
        y += lh;
    }
}

void dialog_frame(const Board& b, int x0, int y0, int x1, int y1, const std::string& title,
                  const std::string& body) {
    draw::fill_a(b.out, b.w, b.h, 0, 0, int(b.w), int(b.h), 0, 0, 0, theme::kAlphaScrim);
    panel_grad(b, x0, y0, x1, y1, theme::kSurfaceRaised, theme::kSurface, 255, theme::kBorderStrong);
    draw::fill_a(b.out, b.w, b.h, x0, y0, x1, y0 + 1, theme::kAccent.r, theme::kAccent.g, theme::kAccent.b, 128);
    const int th = draw::text_h_em(type_px(b, theme::kTypeDialogTitle));
    text_box(b, x0, y0 + b.s(theme::kDialogPadTop), x1, y0 + b.s(theme::kDialogPadTop) + th, title,
             theme::kTypeDialogTitle, theme::kTextHi);
    if (!body.empty())
        text_box(b, x0, y0 + b.s(theme::kDialogPadTop) + th + b.s(theme::kDialogTitleGap), x1,
                 y0 + b.s(theme::kDialogPadTop) + th + b.s(theme::kDialogTitleGap) + draw::text_h_em(type_px(b, theme::kTypeBody)),
                 body, theme::kTypeBody, theme::kTextMuted);
}

void list_row(const Board& b, int x0, int y0, int x1, int y1, const std::string& label, bool selected,
              const Look& k, const std::string& right = std::string()) {
    const theme::Rgb fill = selected ? theme::kSurfaceSel : mix_rgb(theme::kSurfaceRow, theme::kSurfaceSel, k.hover * 0.6f);
    const theme::Rgb edge = selected ? theme::kAccent : mix_rgb(theme::kBorderFaint, theme::kAccentHot, k.hover);
    panel(b, x0, y0, x1, y1, fill, theme::kAlphaPanelDim, edge);
    if (k.pressed) draw::fill_a(b.out, b.w, b.h, x0 + 1, y0 + 1, x1 - 1, y1 - 1, 0, 0, 0, 70);
    if (k.focused) apply_look(b, x0, y0, x1, y1, Look{0.0f, false, true, k.pulse});
    const int pad = b.s(theme::kMapRowPadX);
    int rw = 0;
    if (!right.empty()) {
        rw = run_w(b, right, theme::kTypeMicro) + pad;
        text_left(b, x1 - rw, y0, y1, right, theme::kTypeMicro, theme::kTextFaint, x1 - pad);
    }
    text_left(b, x0 + pad, y0, y1, label, theme::kTypeSmall, selected || k.hover > 0.5f ? theme::kTextHi : theme::kTextCell,
              x1 - pad - rw);
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
            if (c.live && (c.look.hover > 0.0f || c.look.focused)) {
                const int uw = int(float(c.x1 - c.x0) * (c.look.focused ? 1.0f : c.look.hover));
                draw::fill_a(b.out, b.w, b.h, c.x0 + (c.x1 - c.x0 - uw) / 2, navh - 1 - b.s(theme::kTabRuleH),
                             c.x0 + (c.x1 - c.x0 + uw) / 2, navh - 1, theme::kAccent.r, theme::kAccent.g,
                             theme::kAccent.b, 255);
            }
            text_box(b, c.x0, c.y0, c.x1, c.y1, c.label, theme::kTypeNavItem,
                     c.live ? (c.look.hover > 0.5f || c.look.focused ? theme::kTextHi : theme::kTextBody)
                            : theme::kTextMuted2);
        } else {
            button_menu(b, c.x0, c.y0, c.x1, c.y1, c.label, false, c.live, c.look);
        }
    }
    band_footer(b);
}

void draw_play_hub(const Board& b, const std::vector<Ctl>& ctls) {
    band_flow_strip(b, "PLAY");
    const int ty = b.s(theme::kHomeEmblemTop);
    text_box(b, 0, ty, int(b.w), ty + draw::text_h_em(type_px(b, theme::kTypeLoadTitle)), "CHOOSE HOW TO PLAY",
             theme::kTypeLoadTitle, theme::kTextHi);
    struct Face { const char* key; const char* sub; const char* go; };
    static const Face kFaces[] = {
        {"FindMatchButton", "One-on-one against the computer on any map. Pick the races, the tier, the speed.", "-> LOBBY"},
        {"CustomButton", "Every melee map the install ships, plus any folder of your own, with a preview.", "-> CUSTOM GAMES"},
        {"ReplayButton", "Every match this client recorded, played back at its recorded seed.", "-> REPLAYS"},
    };
    for (const Ctl& c : ctls) {
        if (c.x1 <= c.x0 || c.y1 <= c.y0) continue;
        const Face* f = nullptr;
        for (const Face& k : kFaces)
            if (c.key == k.key) f = &k;
        if (f) card(b, c.x0, c.y0, c.x1, c.y1, c.label, f->sub, f->go, c.look);
        else button_secondary(b, c.x0, c.y0, c.x1, c.y1, c.label, theme::kTypeSecondaryLg, c.look);
    }
    band_footer(b);
}

struct ProfileInfo {
    std::string name;
    bool editing = false;
    int matches = 0, wins = 0;
    float caret = 0.0f;
};
void draw_profile(const Board& b, const std::vector<Ctl>& ctls, const ProfileInfo& info) {
    band_flow_strip(b, "PROFILE");
    const int ex0 = b.s(theme::kHomeLeft), ey0 = b.s(theme::kHomeEmblemTop);
    const int esz = b.s(theme::kEmblemSmall);
    image_well(b, ex0, ey0, ex0 + esz, ey0 + esz, shell_emblem(false));
    const int lx = ex0 + esz + b.s(theme::kEmblemGapSmall);
    text_at(b, lx, ey0, info.name, theme::kTypeWordmarkSm, theme::kTextHi);
    text_at(b, lx, ey0 + draw::text_h_em(type_px(b, theme::kTypeWordmarkSm)) + b.s(6),
            std::to_string(info.matches) + " matches played, " + std::to_string(info.wins) + " won",
            theme::kTypeBody, theme::kTextMuted);
    const int cx0 = ex0, cy0 = b.s(theme::kHomeCardTop), cx1 = ex0 + b.s(theme::kHomeCardW);
    const int cy1 = cy0 + b.s(theme::kHomeCardPadY) * 2 + b.s(theme::kRowH) * 3;
    panel(b, cx0, cy0, cx1, cy1, theme::kSurfaceSel, theme::kAlphaCard, theme::kBorderSlot);
    int ry = cy0 + b.s(theme::kHomeCardPadY);
    const int rh = b.s(theme::kRowH), px = b.s(theme::kHomeCardPadX);
    const int lost = info.matches - info.wins;
    const int pct = info.matches > 0 ? (info.wins * 100 + info.matches / 2) / info.matches : 0;
    struct Row { std::string k, v; };
    const Row rows[] = {{"MATCHES", std::to_string(info.matches)},
                        {"WON / LOST", std::to_string(info.wins) + " / " + std::to_string(lost)},
                        {"WIN RATE", std::to_string(pct) + " %"}};
    for (const Row& r : rows) {
        text_left(b, cx0 + px, ry, ry + rh, r.k, theme::kTypeCat, theme::kTextMuted2, cx0 + (cx1 - cx0) / 2);
        text_left(b, cx0 + (cx1 - cx0) / 2, ry, ry + rh, r.v, theme::kTypeBody, theme::kTextCell, cx1 - px);
        draw::fill_a(b.out, b.w, b.h, cx0 + px, ry + rh - 1, cx1 - px, ry + rh, theme::kBorderRow.r, theme::kBorderRow.g,
                     theme::kBorderRow.b, 255);
        ry += rh;
    }
    for (const Ctl& c : ctls) {
        if (c.x1 <= c.x0 || c.y1 <= c.y0) continue;
        if (c.action == kActProfileName) {
            panel(b, c.x0, c.y0, c.x1, c.y1, theme::kSurfaceCtl, 255, info.editing ? theme::kAccent : theme::kBorder);
            apply_look(b, c.x0, c.y0, c.x1, c.y1, c.look);
            const int pad = b.s(theme::kPulldownPadX);
            const int pen = draw::text_w_em(info.name, type_px(b, theme::kTypeSmall), theme::kTypeSmall.track);
            text_left(b, c.x0 + pad, c.y0, c.y1, info.name, theme::kTypeSmall, theme::kTextHi, c.x1 - pad);
            if (info.editing && info.caret > 0.5f)
                draw::fill_a(b.out, b.w, b.h, c.x0 + pad + pen + 1, c.y0 + b.s(8), c.x0 + pad + pen + 2, c.y1 - b.s(8),
                             theme::kAccent.r, theme::kAccent.g, theme::kAccent.b, 255);
            const int ch = draw::text_h_em(type_px(b, theme::kTypeCaption));
            text_at(b, c.x0, c.y0 - ch - b.s(3), info.editing ? "NAME  (type letters, Backspace, Space to finish)" : "NAME  (click to edit)",
                    theme::kTypeCaption, theme::kTextFaint, c.x1);
        } else if (c.action == kActProfileRace) {
            pulldown(b, c.x0, c.y0, c.x1, c.y1, c.label, theme::kTextBody, c.look, "PREFERRED RACE");
        } else if (c.action == kActBack) {
            button_secondary(b, c.x0, c.y0, c.x1, c.y1, c.label, theme::kTypeSecondaryLg, c.look);
        } else {
            button_menu(b, c.x0, c.y0, c.x1, c.y1, c.label, false, c.live, c.look);
        }
    }
    band_footer(b);
}

void draw_quit_dialog(const Board& b, const std::vector<Ctl>& ctls) {
    const int dw = b.s(theme::kDialogW);
    const int dh = b.s(theme::kDialogPadTop) + draw::text_h_em(type_px(b, theme::kTypeDialogTitle)) +
                   b.s(theme::kDialogTitleGap) + draw::text_h_em(type_px(b, theme::kTypeBody)) + b.s(theme::kDialogTitleGap) +
                   b.s(theme::kMenuBtnH) + b.s(theme::kDialogPadBottom);
    const int x0 = (int(b.w) - dw) / 2, y0 = (int(b.h) - dh) / 2;
    dialog_frame(b, x0, y0, x0 + dw, y0 + dh, "LEAVE THE CLIENT?", "Unsaved lobby choices are kept in the config.");
    for (const Ctl& c : ctls) {
        if (c.action == kActQuitYes) button_primary(b, c.x0, c.y0, c.x1, c.y1, c.label, theme::kTypePrimaryMd, c.look);
        else if (c.action == kActQuitNo) button_secondary(b, c.x0, c.y0, c.x1, c.y1, c.label, theme::kTypeSecondary, c.look);
    }
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

struct CustomInfo {
    std::string selected;
    std::string size_line;
    std::string count_line;
    std::string error;
    const IconAtlas::Icon* preview = nullptr;
    bool preview_pending = false;
};
void draw_custom(const Board& b, const std::vector<Ctl>& ctls, const CustomInfo& info) {
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
        text_left(b, pad + tw + b.s(theme::kTabPadX), tabs_y, tabs_y + tab_h, info.count_line, theme::kTypeMicro,
                  theme::kTextFaint, int(b.w) - pad);
    }
    const int top = b.s(theme::kCustomListTop);
    const int bot = b.by(theme::kCustomBottomGap);
    const int detail_x0 = b.rx(theme::kScreenPad) - b.s(theme::kCustomDetailW);
    panel(b, pad, top, pad + b.s(theme::kCustomListW), bot, theme::kSurface, theme::kAlphaPanelDim,
          theme::kBorderFaint);
    panel(b, detail_x0, top, b.rx(theme::kScreenPad), bot, theme::kSurface, theme::kAlphaPanel,
          theme::kBorder);
    const int pv = b.s(theme::kCustomPreview), dp = b.s(theme::kCustomDetailPad);
    if (info.preview) image_well(b, detail_x0 + dp, top + dp, detail_x0 + dp + pv, top + dp + pv, *info.preview);
    else image_well(b, detail_x0 + dp, top + dp, detail_x0 + dp + pv, top + dp + pv);
    if (!info.preview)
        text_box(b, detail_x0 + dp, top + dp, detail_x0 + dp + pv, top + dp + pv,
                 info.preview_pending ? "reading the minimap..." : "no minimap in this archive", theme::kTypeCaption,
                 theme::kTextFaint);
    int iy = top + dp + pv + dp;
    text_at(b, detail_x0 + dp, iy, info.selected, theme::kTypeDetailTitle, theme::kTextHi, b.rx(theme::kScreenPad) - dp);
    iy += draw::text_h_em(type_px(b, theme::kTypeDetailTitle)) + b.s(8);
    text_at(b, detail_x0 + dp, iy, info.size_line, theme::kTypeSmall, theme::kTextMuted, b.rx(theme::kScreenPad) - dp);
    if (!info.error.empty()) {
        iy += draw::text_h_em(type_px(b, theme::kTypeSmall)) + b.s(8);
        text_at(b, detail_x0 + dp, iy, info.error, theme::kTypeMicro, theme::kErr, b.rx(theme::kScreenPad) - dp);
    }
    for (const Ctl& c : ctls) {
        if (c.x1 <= c.x0 || c.y1 <= c.y0) continue;
        if (c.key == "CreateButton") button_primary(b, c.x0, c.y0, c.x1, c.y1, c.label, theme::kTypePrimaryMd, c.look);
        else if (c.key == "BackButton") button_secondary(b, c.x0, c.y0, c.x1, c.y1, c.label, theme::kTypeSecondaryMd, c.look);
        else if (c.action == kActMapsFolder) button_secondary(b, c.x0, c.y0, c.x1, c.y1, c.label, theme::kTypeSecondaryMd, c.look);
        else if (c.action >= kMapRow0) list_row(b, c.x0, c.y0, c.x1, c.y1, c.label, c.selected, c.look);
        else pulldown(b, c.x0, c.y0, c.x1, c.y1, c.label, theme::kTextBody, c.look);
    }
    band_footer(b);
}

struct OptionsInfo {
    int selected_rail = 0;
    bool engine_page = false;
    int documents = 0;
    std::vector<std::string> row_values;
    std::vector<float> row_fracs;
};
void draw_options(const Board& b, const std::vector<Ctl>& ctls, const OptionsInfo& info) {
    band_flow_strip(b, "OPTIONS");
    const int dw = b.s(theme::kOptionsW), dh = b.s(theme::kOptionsH);
    const int x0 = (int(b.w) - dw) / 2, y0 = b.s(theme::kOptionsTop);
    const int x1 = x0 + dw, y1 = y0 + dh;
    panel(b, x0, y0, x1, y1, theme::kSurface, 255, theme::kBorderStrong);
    const int rail = x0 + b.s(theme::kOptionsRailW);
    draw::fill_a(b.out, b.w, b.h, rail, y0 + 1, rail + 1, y1 - 1, theme::kSurfaceHi.r,
                 theme::kSurfaceHi.g, theme::kSurfaceHi.b, 255);
    for (const Ctl& c : ctls) {
        if (c.action < kActOptRail0 || c.action >= kActOptRow0) continue;
        const int i = c.action == kActOptEngine ? kRailEngine : c.action - kActOptRail0;
        const bool on = i == info.selected_rail;
        if (on || c.look.hover > 0.0f || c.look.focused) {
            const std::uint8_t a = on ? theme::kAlphaCatActive : std::uint8_t(float(theme::kAlphaCatActive) * 0.6f * std::max(c.look.hover, c.look.focused ? 1.0f : 0.0f));
            draw::fill_a(b.out, b.w, b.h, c.x0, c.y0, c.x1, c.y1, theme::kSurfaceSel.r, theme::kSurfaceSel.g,
                         theme::kSurfaceSel.b, a);
        }
        if (on)
            draw::fill_a(b.out, b.w, b.h, c.x0, c.y0, c.x0 + b.s(theme::kCatRuleW), c.y1, theme::kAccent.r,
                         theme::kAccent.g, theme::kAccent.b, 255);
        if (c.look.focused) apply_look(b, c.x0, c.y0, c.x1, c.y1, Look{0.0f, false, true, c.look.pulse});
        text_left(b, c.x0 + b.s(theme::kCatPadX), c.y0, c.y1, c.label, theme::kTypeCat,
                  on || c.look.hover > 0.5f ? theme::kTextHi : theme::kTextMuted, c.x1);
    }
    band_footer(b);
    if (info.engine_page) {
        const int px = b.s(theme::kOptionsPadX), py = b.s(theme::kOptionsPadY);
        text_at(b, rail + px, y0 + py, "ENGINE", theme::kTypeSectionHead, theme::kAccent, x1 - px);
        text_at(b, rail + px, y0 + py + draw::text_h_em(type_px(b, theme::kTypeSectionHead)) + b.s(4),
                "Every row here is honoured by this client and saved as you change it. Left / Right nudge the focused row.",
                theme::kTypeMicro, theme::kTextMuted, x1 - px);
        const std::vector<EngineRow>& rows = engine_rows();
        for (const Ctl& c : ctls) {
            if (c.action < kActOptRow0 || c.action - kActOptRow0 >= int(rows.size())) continue;
            const std::size_t i = std::size_t(c.action - kActOptRow0);
            const EngineRow& r = rows[i];
            const std::string v = i < info.row_values.size() ? info.row_values[i] : std::string();
            const float f = i < info.row_fracs.size() ? info.row_fracs[i] : 0.0f;
            if (r.kind == EngineRow::Kind::Slider) {
                slider(b, c.x0, c.y0, c.x1, c.y1, r.label, f, v, c.look);
            } else {
                if (c.look.focused || c.look.hover > 0.0f)
                    draw::fill_a(b.out, b.w, b.h, c.x0, c.y0, c.x1, c.y1, theme::kSurfaceSel.r, theme::kSurfaceSel.g,
                                 theme::kSurfaceSel.b, std::uint8_t(60 + int(60.0f * c.look.hover)));
                text_left(b, c.x0 + b.s(theme::kPulldownPadX), c.y0, c.y1, r.label, theme::kTypeBody, theme::kTextBody,
                          c.x0 + (c.x1 - c.x0) / 2);
                const int wx0 = c.x0 + (c.x1 - c.x0) / 2, wy0 = c.y0 + (c.y1 - c.y0 - b.s(theme::kPulldownH)) / 2;
                if (r.kind == EngineRow::Kind::Pulldown) {
                    pulldown(b, wx0, wy0, c.x1 - b.s(theme::kPulldownPadX), wy0 + b.s(theme::kPulldownH), v, theme::kTextCell,
                             Look{c.look.hover, c.look.pressed, false, 0.0f});
                } else {
                    const int cb = b.s(18), cy = (c.y0 + c.y1) / 2 - cb / 2;
                    panel(b, wx0, cy, wx0 + cb, cy + cb, theme::kSurfaceDim, 255,
                          mix_rgb(theme::kBorderCtl, theme::kAccentHot, c.look.hover));
                    if (f > 0.5f)
                        draw::fill_a(b.out, b.w, b.h, wx0 + cb / 4, cy + cb / 4, wx0 + cb - cb / 4, cy + cb - cb / 4,
                                     theme::kAccent.r, theme::kAccent.g, theme::kAccent.b, 255);
                    text_left(b, wx0 + cb + b.s(theme::kPulldownPadX), c.y0, c.y1, v, theme::kTypeSmall, theme::kTextCell, c.x1);
                }
                if (c.look.focused) apply_look(b, c.x0, c.y0, c.x1, c.y1, Look{0.0f, false, true, c.look.pulse});
            }
        }
        for (const Ctl& c : ctls)
            if (c.action == 0) button_menu(b, c.x0, c.y0, c.x1, c.y1, c.label, false, c.live, c.look);
        return;
    }
    for (const Ctl& c : ctls)
        if (c.action == 0) button_menu(b, c.x0, c.y0, c.x1, c.y1, c.label, false, c.live, c.look);
    const std::string note = "the OptionsDialog + its " + std::string(kRailNames[info.selected_rail < 0 ? 0 : info.selected_rail]) +
                             " panel (of " + std::to_string(std::max(0, info.documents - 1)) +
                             " panel documents, one shown at a time) - the ENGINE row holds the settings this client applies";
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
    float pulse = 0.0f;
    std::string difficulty, speed;
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
        if (fill < bw) {
            const int sw = std::max(4, bw / 12);
            const int sx = bx0 + fill + int(float(bw - fill - sw) * info.pulse);
            draw::fill_a(b.out, b.w, b.h, std::max(bx0 + fill, sx), by0 + 1, std::min(bx0 + bw, sx + sw), by0 + bh - 1,
                         theme::kAccentHot.r, theme::kAccentHot.g, theme::kAccentHot.b, 70);
        }
        text_at(b, bx0, by0 + bh + b.s(6),
                info.stage + "  -  " + std::to_string(info.done) + " / " +
                    std::to_string(info.total),
                theme::kTypeCaption, theme::kTextFaint, bx0 + bw);
    } else {
        text_at(b, bx0, by0 + bh + b.s(6), "Preparing the load plan", theme::kTypeCaption, theme::kTextFaint, bx0 + bw);
    }
    if (!info.difficulty.empty()) {
        const std::string line = "A.I. " + info.difficulty + "   -   speed " + info.speed + "   -   Esc cancels";
        text_box(b, 0, by0 - b.s(theme::kRowH), int(b.w), by0 - b.s(theme::kRowH) + draw::text_h_em(type_px(b, theme::kTypeSmall)),
                 line, theme::kTypeSmall, theme::kTextMuted);
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
    std::string host_name;
    const IconAtlas::Icon* preview = nullptr;
    bool preview_pending = false;
};
void draw_lobby(const Board& b, const std::vector<Ctl>& ctls, const LobbyInfo& info, int act_start,
                int act_back, int act_folder, int map_row0) {
    band_nav(b);
    const int pad = b.s(theme::kScreenPad);
    text_at(b, pad, b.s(14), "GAME LOBBY", theme::kTypePanelTitle, theme::kTextHi);
    text_at(b, pad + run_w(b, "GAME LOBBY", theme::kTypePanelTitle) + b.s(theme::kPagePad), b.s(22),
            "1V1 MELEE  -  Tab / arrows move, Space picks, Esc leaves", theme::kTypeMicro, theme::kTextFaint);

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
                 i == 0 ? info.host_name + "  (Host)" : "A.I. SLOT", theme::kTextBody, Look{},
                 i == 0 ? "PLAYER" : "");
    }
    const int band_y = b.s(theme::kLobbyOptionsTop);
    panel(b, pad, band_y, pad + slots_w, b.by(theme::kFooterH) - b.s(theme::kPagePad),
          theme::kSurface, theme::kAlphaPanelDim, theme::kBorderFaint);
    text_at(b, pad + b.s(theme::kMapRowPadX), band_y - draw::text_h_em(type_px(b, theme::kTypeCaption)) - b.s(3),
            "MAP  -  wheel / Page Up / Page Down scroll the library", theme::kTypeCaption, theme::kTextFaint,
            pad + slots_w);

    const int mi_x1 = b.rx(theme::kScreenPad), mi_x0 = mi_x1 - b.s(theme::kMapInfoW);
    const int mi_h = b.s(theme::kMinimapBox) + b.s(theme::kMapInfoPad) * 2;
    panel(b, mi_x0, slots_y, mi_x1, slots_y + mi_h, theme::kSurface, theme::kAlphaPanel,
          theme::kBorder);
    const int mm = b.s(theme::kMinimapBox), mp = b.s(theme::kMapInfoPad);
    if (info.preview) image_well(b, mi_x0 + mp, slots_y + mp, mi_x0 + mp + mm, slots_y + mp + mm, *info.preview);
    else {
        image_well(b, mi_x0 + mp, slots_y + mp, mi_x0 + mp + mm, slots_y + mp + mm);
        text_box(b, mi_x0 + mp, slots_y + mp, mi_x0 + mp + mm, slots_y + mp + mm,
                 info.preview_pending ? "reading..." : "no minimap", theme::kTypeCaption, theme::kTextFaint);
    }
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
            button_primary(b, c.x0, c.y0, c.x1, c.y1, c.label, theme::kTypePrimaryLg, c.look);
        } else if (c.action == act_back) {
            button_secondary(b, c.x0, c.y0, c.x1, c.y1, c.label, theme::kTypeSecondaryLg, c.look);
        } else if (c.action == act_folder) {
            button_secondary(b, c.x0, c.y0, c.x1, c.y1, c.label, theme::kTypeSecondary, c.look);
        } else if (c.action >= map_row0) {
            list_row(b, c.x0, c.y0, c.x1, c.y1, c.label, c.selected, c.look);
        } else if (c.action == kActP0Race || c.action == kActP1Race) {
            pulldown(b, c.x0, c.y0, c.x1, c.y1, c.label, theme::kTextBody, c.look, c.action == kActP0Race ? "RACE" : "");
        } else if (c.action == kActAiDiff) {
            pulldown(b, c.x0, c.y0, c.x1, c.y1, c.label, theme::kTextBody, c.look, "");
        } else if (c.action == kActSpeed) {
            pulldown(b, c.x0, c.y0, c.x1, c.y1, c.label, theme::kTextBody, c.look, "");
        } else {
            pulldown(b, c.x0, c.y0, c.x1, c.y1, c.label, theme::kTextBody, c.look);
        }
    }
    for (const Ctl& c : ctls) {
        if (c.action != kActAiDiff && c.action != kActSpeed) continue;
        const int ch = draw::text_h_em(type_px(b, theme::kTypeCaption));
        text_at(b, c.x0, slots_y - ch - b.s(3), c.action == kActAiDiff ? "A.I. DIFFICULTY" : "GAME SPEED",
                theme::kTypeCaption, theme::kTextFaint, c.x1);
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
    menu_ctl_.mirror_x = false;
    maps_.push_back({"", "Synthetic Arena (72x72)  [built-in, no cliffs]"});
}

void FrontEnd::set_maps(std::vector<platform::MapEntry> maps) {
    maps_.assign(1, platform::MapEntry{"", "Synthetic Arena (72x72)  [built-in, no cliffs]"});
    for (auto& m : maps) maps_.push_back(std::move(m));
    sel_map_ = maps_.size() > 1 ? 1 : 0;
    thumb_cursor_ = 0;
    const std::string stored = shell_v1() ? std::string() : settings_.lobby_map();
    if (!stored.empty())
        for (std::size_t i = 1; i < maps_.size(); ++i)
            if (maps_[i].path == stored) sel_map_ = int(i);
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

    const float shell_ref = float(app::theme::kDesignRefH);
    const float ref_scale = float(view_h) / shell_ref;
    const ui::Rect canvas{0, 0, shell_ref * float(view_w) / float(view_h), shell_ref};
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
        {"VERSUS A.I.", "PlayButton"},
        {"CUSTOM GAME", "CustomButton"},
        {"REPLAYS", "ReplayButton"},
        {"SANDBOX", "SandboxButton"},
        {"PROFILE", "ProfileButton"},
        {"OPTIONS", "menu/Options"},
        {"CREDITS", "Credits entry"},
        {"QUIT", "Quit game"},
    };
    const int bw = theme::scaled(theme::kMenuBtnW, view_h);
    const int bh = theme::scaled(theme::kMenuBtnH, view_h);
    const int gap = theme::scaled(theme::kMenuBtnGap, view_h);
    const int x0 = theme::scaled(theme::kMainEmblemLeft, view_h);
    int y = theme::scaled(theme::kMainContentTop, view_h);
    for (const Button& b : out)
        if (b.y1 + gap > y && b.x1 > x0 && b.x0 < x0 + bw) y = b.y1 + gap;
    for (const OwnEntry& e : kOwn) {
        out.push_back({x0, y, x0 + bw, y + bh, e.label, e.trigger, e.trigger == std::string("Quit game") ? kActQuit : 0});
        y += bh + gap;
    }
    return out;
}

std::vector<FrontEnd::Button> FrontEnd::quit_dialog_buttons(std::uint32_t view_w, std::uint32_t view_h) const {
    std::vector<Button> out;
    auto S = [view_h](int a) { return theme::scaled(a, view_h); };
    const int dw = S(theme::kDialogW);
    const int th = S(20), bh_body = S(13);
    const int dh = S(theme::kDialogPadTop) + th + S(theme::kDialogTitleGap) + bh_body + S(theme::kDialogTitleGap) +
                   S(theme::kMenuBtnH) + S(theme::kDialogPadBottom);
    const int x0 = (int(view_w) - dw) / 2, y0 = (int(view_h) - dh) / 2;
    const int by = y0 + dh - S(theme::kDialogPadBottom) - S(theme::kMenuBtnH);
    const int col = S(theme::kMenuBtnW), gap = S(theme::kActionGap);
    const int half = (col - gap) / 2, cx0 = x0 + S(theme::kDialogPadX);
    out.push_back({cx0, by, cx0 + half, by + S(theme::kMenuBtnH), "LEAVE", "Confirm quit", kActQuitYes});
    out.push_back({cx0 + half + gap, by, cx0 + col, by + S(theme::kMenuBtnH), "STAY", "Cancel quit", kActQuitNo});
    return out;
}

std::vector<FrontEnd::Button> FrontEnd::play_hub_buttons(std::uint32_t view_w, std::uint32_t view_h) const {
    std::vector<Button> out;
    auto S = [view_h](int a) { return theme::scaled(a, view_h); };
    const int tw = S(theme::kTileW), th = S(theme::kTileH), gap = S(theme::kTileGap);
    const int total = tw * 3 + gap * 2;
    int x = (int(view_w) - total) / 2;
    const int y = S(theme::kHomeCardTop);
    struct Tile { const char* label; const char* key; };
    static const Tile kTiles[] = {{"VERSUS A.I.", "FindMatchButton"}, {"CUSTOM GAME", "CustomButton"}, {"REPLAYS", "ReplayButton"}};
    for (const Tile& t : kTiles) {
        out.push_back({x, y, x + tw, y + th, t.label, t.key, 0});
        x += tw + gap;
    }
    const int bw = S(theme::kPrimaryW), bh = S(theme::kPrimaryH);
    const int bx = (int(view_w) - bw) / 2, by = y + th + S(theme::kHomeCardGap) * 2;
    out.push_back({bx, by, bx + bw, by + bh, "BACK", "BackButton", 0});
    return out;
}

int FrontEnd::custom_rows_per_page(std::uint32_t view_h) const {
    const int pitch = theme::scaled(theme::kPulldownH, view_h) + theme::scaled(theme::kMapRowGap, view_h);
    const int top = theme::scaled(theme::kCustomListTop, view_h) + theme::scaled(theme::kMapRowPadY, view_h);
    const int bottom = int(view_h) - theme::scaled(theme::kCustomBottomGap, view_h) - pitch * 2;
    return std::max(1, (bottom - top) / std::max(1, pitch));
}

void FrontEnd::clamp_custom_scroll(std::uint32_t view_h) {
    const int max_scroll = std::max(0, int(maps_.size()) - custom_rows_per_page(view_h));
    custom_scroll_ = std::max(0, std::min(custom_scroll_, max_scroll));
}

std::vector<FrontEnd::Button> FrontEnd::custom_buttons(std::uint32_t view_w, std::uint32_t view_h) const {
    std::vector<Button> out;
    auto S = [view_h](int a) { return theme::scaled(a, view_h); };
    const int pad = S(theme::kScreenPad);
    const int lx0 = pad + S(theme::kMapRowPadX), lx1 = pad + S(theme::kCustomListW) - S(theme::kMapRowPadX);
    const int listy = S(theme::kCustomListTop) + S(theme::kMapRowPadY);
    const int pd_h = S(theme::kPulldownH), pitch = pd_h + S(theme::kMapRowGap);
    const int page = custom_rows_per_page(view_h);
    for (int i = 0; i < page && custom_scroll_ + i < int(maps_.size()); ++i) {
        const int y = listy + i * pitch;
        out.push_back({lx0, y, lx1, y + pd_h, maps_[std::size_t(custom_scroll_ + i)].name, "", kMapRow0 + custom_scroll_ + i});
    }
    if (int(maps_.size()) > page) {
        const int sy = listy + page * pitch;
        const int half = (lx1 - lx0 - S(theme::kSlotGapX)) / 2;
        out.push_back({lx0, sy, lx0 + half, sy + pd_h, "Prev", "", kActScrollUp});
        out.push_back({lx1 - half, sy, lx1, sy + pd_h, "Next", "", kActScrollDown});
    }
    const int dx1 = int(view_w) - pad, dx0 = dx1 - S(theme::kCustomDetailW), dp = S(theme::kCustomDetailPad);
    const int bot = int(view_h) - S(theme::kCustomBottomGap);
    const int ah = S(theme::kActionH);
    const int cw = label_w(view_h, "CREATE GAME", theme::kTypePrimaryMd) + S(theme::kActionPadX) * 2;
    out.push_back({dx1 - dp - cw, bot - dp - ah, dx1 - dp, bot - dp, "CREATE GAME", "CreateButton", 0});
    if (maps_folder_cb_) {
        const int fw = label_w(view_h, "MAPS FOLDER...", theme::kTypeSecondaryMd) + S(theme::kActionPadX) * 2;
        out.push_back({dx1 - dp - cw - S(theme::kActionGap) - fw, bot - dp - ah, dx1 - dp - cw - S(theme::kActionGap), bot - dp,
                       "MAPS FOLDER...", "", kActMapsFolder});
    }
    const int bw = S(theme::kPrimaryW);
    out.push_back({dx0 + dp, bot - dp - ah, dx0 + dp + bw, bot - dp, "BACK", "BackButton", 0});
    return out;
}

std::vector<FrontEnd::Button> FrontEnd::profile_buttons(std::uint32_t view_w, std::uint32_t view_h) const {
    std::vector<Button> out;
    auto S = [view_h](int a) { return theme::scaled(a, view_h); };
    const int x0 = S(theme::kHomeLeft) + S(theme::kHomeCardW) + S(theme::kHomeCardGap);
    const int y0 = S(theme::kHomeCardTop) + S(theme::kHomeCardPadY) + S(12);
    const int w = S(theme::kColPlayer) + S(theme::kColRace);
    out.push_back({x0, y0, x0 + w, y0 + S(theme::kPulldownH), settings_.profile_name(), "Profile name", kActProfileName});
    const int y1 = y0 + S(theme::kPulldownH) + S(theme::kRowH);
    out.push_back({x0, y1, x0 + w, y1 + S(theme::kPulldownH), lobby_race_name(settings_.profile_race()), "Profile race", kActProfileRace});
    const int bw = S(theme::kPrimaryW), bh = S(theme::kPrimaryH);
    const int bx1 = int(view_w) - S(theme::kScreenPad), by = int(view_h) - S(theme::kFooterH) - S(theme::kPagePad) - bh;
    out.push_back({bx1 - bw, by, bx1, by + bh, "BACK", "BackButton", kActBack});
    return out;
}

std::vector<FrontEnd::Button> FrontEnd::options_shell_buttons(std::uint32_t view_w, std::uint32_t view_h,
                                                              const std::vector<Button>& authored) const {
    auto free_slot = [&authored](int x0, int y0, int x1, int y1) {
        const float cx = float(x0 + x1) * 0.5f, cy = float(y0 + y1) * 0.5f;
        for (const Button& a : authored) {
            if (a.x1 <= a.x0 || a.y1 <= a.y0) continue;
            const float ax = float(a.x0 + a.x1) * 0.5f, ay = float(a.y0 + a.y1) * 0.5f;
            if (ax >= float(x0) && ax < float(x1) && ay >= float(y0) && ay < float(y1)) return false;
            if (a.hit(cx, cy)) return false;
        }
        return true;
    };
    std::vector<Button> out;
    auto S = [view_h](int a) { return theme::scaled(a, view_h); };
    const int dw = S(theme::kOptionsW);
    const int x0 = (int(view_w) - dw) / 2, y0 = S(theme::kOptionsTop);
    const int y1 = y0 + S(theme::kOptionsH);
    const int rail = x0 + S(theme::kOptionsRailW);
    const int cat_h = S(theme::kCatPadY) * 2 + S(12);
    int cy = y0 + S(theme::kOptionsRailPadY);
    for (int i = 0; i < kRailCount; ++i) {
        const std::string key = i == kRailHotkeys ? std::string("HotkeysButton") : std::string("rail/") + kRailNames[i];
        if (i == kRailEngine) {
            int ey = cy;
            while (ey + cat_h <= y1 && !free_slot(x0 + 1, ey, rail, ey + cat_h)) ey += cat_h;
            if (ey + cat_h > y1) ey = y1 - cat_h;
            out.push_back({x0 + 1, ey, rail, ey + cat_h, kRailNames[i], key, kActOptEngine});
        } else if (free_slot(x0 + 1, cy, rail, cy + cat_h)) {
            out.push_back({x0 + 1, cy, rail, cy + cat_h, kRailNames[i], key, kActOptRail0 + i});
        }
        cy += cat_h;
    }
    if (options_engine_page_) {
        const int px = S(theme::kOptionsPadX), py = S(theme::kOptionsPadY);
        const int rx0 = rail + px, rx1 = x0 + dw - px;
        int ry = y0 + py + S(17) + S(4) + S(11) + S(theme::kPagePad);
        const int rh = S(theme::kRowH);
        const std::vector<EngineRow>& rows = engine_rows();
        for (std::size_t i = 0; i < rows.size(); ++i) {
            while (ry + rh <= y1 && !free_slot(rx0, ry, rx1, ry + rh)) ry += rh + S(theme::kMapRowGap);
            if (ry + rh > y1) break;
            out.push_back({rx0, ry, rx1, ry + rh, rows[i].label, rows[i].key, kActOptRow0 + int(i)});
            ry += rh + S(theme::kMapRowGap);
        }
    }
    return out;
}

std::vector<FrontEnd::Button> FrontEnd::screen_buttons(std::uint32_t view_w, std::uint32_t view_h) {
    const ScreenId id = router_.top();
    if (id == ScreenId::GlueRoot) return quit_confirm_ ? quit_dialog_buttons(view_w, view_h) : menu_buttons(view_w, view_h);
    if (id == ScreenId::Lobby) return lobby_buttons(view_w, view_h);
    if (id == ScreenId::ReplayBrowse) return replay_buttons(view_w, view_h);
    if (id == ScreenId::VersusAI) return play_hub_buttons(view_w, view_h);
    if (id == ScreenId::Custom) return custom_buttons(view_w, view_h);
    if (id == ScreenId::Profile) return profile_buttons(view_w, view_h);
    std::vector<Button> out;
    if (const GlueScreen* s = glue_screen(id, view_w, view_h))
        for (std::size_t i = 0; i < s->controls().size(); ++i) {
            const ui::Rect& r = s->hit_rect(i);
            const ui::SolvedFrame& f = s->controls()[i];
            out.push_back({int(r.x), int(r.y), int(r.x + r.w), int(r.y + r.h), f.text.empty() ? f.name : f.text, f.name, 0});
        }
    if (id == ScreenId::Options) {
        const std::vector<Button> shell = options_shell_buttons(view_w, view_h, out);
        out.insert(out.end(), shell.begin(), shell.end());
    }
    if (id == ScreenId::Home) {
        auto S = [view_h](int a) { return theme::scaled(a, view_h); };
        const int x0 = S(theme::kHomeLeft), y0 = S(theme::kHomeCardTop);
        out.push_back({x0, y0, x0 + S(theme::kHomeCardW), y0 + S(theme::kHomeCardPadY) * 2 + S(theme::kHomeThumb), "PLAY", "PlayButton", 0});
    }
    return out;
}

void FrontEnd::activate(const Button& b, float mx, std::uint32_t view_w, std::uint32_t view_h) {
    (void)view_w;
    const ScreenId id = router_.top();
    auto follow = [&](const std::string& control) { router_.follow(trigger_for_control(id, control)); };
    if (id == ScreenId::GlueRoot) {
        if (quit_confirm_) {
            if (b.action == kActQuitYes) { quit_confirm_ = false; router_.follow("Esc / close client"); }
            else if (b.action == kActQuitNo) quit_confirm_ = false;
            return;
        }
        if (b.action == kActQuit) { quit_confirm_ = true; fx_.focus = -1; return; }
        follow(b.trigger);
        return;
    }
    if (id == ScreenId::Lobby) {
        if (b.action >= kMapRow0) {
            sel_map_ = b.action - kMapRow0;
            match_error_.clear();
            if (!shell_v1() && sel_map_ >= 0 && sel_map_ < int(maps_.size())) settings_.set_lobby_map(maps_[std::size_t(sel_map_)].path);
        }
        else if (b.action == kActP0Race) {
            p0_pick_ = lobby_race_cycle(p0_pick_);
            if (p0_pick_ != LobbyRace::Random) slots_[0].race = Race(int(p0_pick_));
            settings_.set(ShellSettings::kProfileRaceKey, std::to_string(int(p0_pick_)));
        } else if (b.action == kActP1Race) {
            p1_pick_ = lobby_race_cycle(p1_pick_);
            if (p1_pick_ != LobbyRace::Random) slots_[1].race = Race(int(p1_pick_));
            if (!shell_v1()) settings_.set_opponent_race(p1_pick_);
        } else if (b.action == kActAiDiff) settings_.cycle_ai_difficulty(1);
        else if (b.action == kActSpeed) settings_.cycle_game_speed(1);
        else if (b.action == kActMapsFolder) {
            if (maps_folder_cb_) maps_folder_cb_();
            if (maps_.size() <= 1) fx_.toast("No .SC2Map found in that folder - the built-in arena only", true);
        }
        else if (b.action == kActScrollUp) { map_scroll_ -= lobby_rows_per_page(view_h); clamp_map_scroll(view_h); }
        else if (b.action == kActScrollDown) { map_scroll_ += lobby_rows_per_page(view_h); clamp_map_scroll(view_h); }
        else if (b.action == kActBack) router_.follow("Back / leave lobby");
        else if (b.action == kActStart) {
            match_seed_ = new_match_seed();
            const std::uint64_t match_seed = match_seed_ ? match_seed_ : MeleeGameConfig{}.seed;
            set_two_player_races(resolve_lobby_race(p0_pick_, match_seed, 0), resolve_lobby_race(p1_pick_, match_seed, 1));
            match_error_.clear();
            loading_frames_ = 0;
            load_plan_.reset();
            router_.follow("StartButton -> countdown");
        }
        return;
    }
    if (id == ScreenId::ReplayBrowse) {
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
                slots_from_log(replay_log_pending_);
                match_error_.clear();
                loading_frames_ = 0;
                load_plan_.reset();
                router_.follow("Watch replay");
            } else {
                match_error_ = "cannot read replay " + replays_[std::size_t(sel_replay_)].name;
                fx_.toast(match_error_, true);
            }
        }
        return;
    }
    if (id == ScreenId::Custom) {
        if (b.action >= kMapRow0) { sel_map_ = b.action - kMapRow0; match_error_.clear(); }
        else if (b.action == kActScrollUp) { custom_scroll_ -= custom_rows_per_page(view_h); clamp_custom_scroll(view_h); }
        else if (b.action == kActScrollDown) { custom_scroll_ += custom_rows_per_page(view_h); clamp_custom_scroll(view_h); }
        else if (b.action == kActMapsFolder) {
            if (maps_folder_cb_) maps_folder_cb_();
            if (maps_.size() <= 1) fx_.toast("No .SC2Map found in that folder - the built-in arena only", true);
        } else follow(b.trigger);
        return;
    }
    if (id == ScreenId::Profile) {
        if (b.action == kActProfileName) profile_editing_ = !profile_editing_;
        else if (b.action == kActProfileRace) {
            settings_.cycle_profile_race(1);
            p0_pick_ = settings_.profile_race();
            if (p0_pick_ != LobbyRace::Random) slots_[0].race = Race(int(p0_pick_));
        } else if (b.action == kActBack) { profile_editing_ = false; router_.follow("BackButton"); }
        return;
    }
    if (id == ScreenId::Options && b.action != 0) {
        if (b.action == kActOptEngine) {
            if (!options_engine_page_) glue_cache_[std::size_t(ScreenId::Options)].tried = false;
            options_engine_page_ = true;
            fx_.focus = -1;
            return;
        }
        if (b.action >= kActOptRail0 && b.action < kActOptEngine) {
            const int i = b.action - kActOptRail0;
            if (i == kRailHotkeys) { router_.follow("HotkeysButton"); return; }
            if (options_engine_page_) glue_cache_[std::size_t(ScreenId::Options)].tried = false;
            options_engine_page_ = false;
            select_options_panel(i < kRailHotkeys ? i : i - 1);
            return;
        }
        if (b.action >= kActOptRow0) {
            const std::vector<EngineRow>& rows = engine_rows();
            const std::size_t i = std::size_t(b.action - kActOptRow0);
            if (i >= rows.size()) return;
            const EngineRow& r = rows[i];
            bool ok = true;
            if (r.kind == EngineRow::Kind::Slider) {
                const int half = b.x0 + (b.x1 - b.x0) / 2;
                const int vw = theme::scaled(64, view_h);
                const float t0 = float(half), t1 = float(b.x1 - vw);
                float f = t1 > t0 ? (mx - t0) / (t1 - t0) : 0.0f;
                f = f < 0.0f ? 0.0f : (f > 1.0f ? 1.0f : f);
                if (std::string(r.key) == "opt/display.ui_scale") ok = settings_.set_ui_scale(kUiScaleMin + f * (kUiScaleMax - kUiScaleMin));
                else
                    for (const ShellSettings::Bus& bus : ShellSettings::buses())
                        if (std::string("opt/") + bus.key == r.key) ok = settings_.set_bus_gain(bus, f);
            } else {
                const std::string k = r.key;
                if (k == "opt/gameplay.unit_status") settings_.cycle_unit_status(1);
                else if (k == "opt/gameplay.game_speed") settings_.cycle_game_speed(1);
                else if (k == "opt/hotkey.profile") settings_.cycle_hotkey_profile(1);
                else if (k == "opt/display.mode") settings_.cycle_display_mode(1);
                else if (k == "opt/display.vsync") settings_.toggle_vsync();
            }
            if (!ok) fx_.toast("The setting could not be written to the config file", true);
            if (skirmish_) apply_settings_to_match();
            return;
        }
    }
    if (id == ScreenId::VersusAI) { follow(b.trigger); return; }
    if (id == ScreenId::Options) {
        if (b.trigger.size() > 6 && b.trigger.compare(b.trigger.size() - 6, 6, "Button") == 0) {
            const std::string panel = b.trigger.substr(0, b.trigger.size() - 6) + "OptionsPanel";
            const std::vector<const char*>& panels = options_panels();
            for (std::size_t i = 0; i < panels.size(); ++i)
                if (panel == panels[i]) {
                    if (options_engine_page_) glue_cache_[std::size_t(ScreenId::Options)].tried = false;
                    options_engine_page_ = false;
                    select_options_panel(int(i));
                    return;
                }
        }
        if (b.trigger.size() > 8 && b.trigger.compare(b.trigger.size() - 8, 8, "CheckBox") == 0) { toggle_option(b.trigger); return; }
    }
    follow(b.trigger);
}

void FrontEnd::shell_keyboard(const render::InputState& in, std::uint32_t view_w, std::uint32_t view_h) {
    const ShellKeys k = ShellKeys::read(in);
    const bool pan_now[4] = {k.left, k.right, k.up, k.down};
    bool edge[4];
    for (int i = 0; i < 4; ++i) { edge[i] = pan_now[i] && !prev_pan_[i]; prev_pan_[i] = pan_now[i]; }
    const ScreenId id = router_.top();
    if (id == ScreenId::Profile && profile_editing_) {
        std::string n = settings_.profile_name();
        bool changed = false;
        if (k.letter && n.size() < 16) { n.push_back(k.letter); changed = true; }
        if (k.backspace && !n.empty()) { n.pop_back(); changed = true; }
        if (changed) settings_.set_profile_name(n);
        if (k.space || k.enter) profile_editing_ = false;
        return;
    }
    std::vector<Button> bs = screen_buttons(view_w, view_h);
    std::vector<ui::Rect> rects;
    rects.reserve(bs.size());
    for (const Button& b : bs) rects.push_back({float(b.x0), float(b.y0), float(b.x1 - b.x0), float(b.y1 - b.y0)});
    if (k.tab) fx_.focus_tab(int(bs.size()), 1);
    if (k.shift_tab) fx_.focus_tab(int(bs.size()), -1);
    const bool focused_slider = fx_.focus >= 0 && fx_.focus < int(bs.size()) && id == ScreenId::Options && bs[std::size_t(fx_.focus)].action >= kActOptRow0;
    const bool focused_picker = fx_.focus >= 0 && fx_.focus < int(bs.size()) &&
                                (bs[std::size_t(fx_.focus)].action == kActP0Race || bs[std::size_t(fx_.focus)].action == kActP1Race ||
                                 bs[std::size_t(fx_.focus)].action == kActAiDiff || bs[std::size_t(fx_.focus)].action == kActSpeed ||
                                 bs[std::size_t(fx_.focus)].action == kActProfileRace);
    if ((edge[0] || edge[1]) && (focused_slider || focused_picker)) {
        const Button& b = bs[std::size_t(fx_.focus)];
        const int dir = edge[1] ? 1 : -1;
        if (focused_slider) {
            const EngineRow& r = engine_rows()[std::size_t(b.action - kActOptRow0)];
            const std::string key = r.key;
            if (r.kind == EngineRow::Kind::Slider) {
                if (key == "opt/display.ui_scale") settings_.set_ui_scale(settings_.ui_scale() + 0.05f * float(dir));
                else
                    for (const ShellSettings::Bus& bus : ShellSettings::buses())
                        if (std::string("opt/") + bus.key == key) settings_.set_bus_gain(bus, settings_.bus_gain(bus) + 0.05f * float(dir));
            } else if (key == "opt/gameplay.unit_status") settings_.cycle_unit_status(dir);
            else if (key == "opt/gameplay.game_speed") settings_.cycle_game_speed(dir);
            else if (key == "opt/hotkey.profile") settings_.cycle_hotkey_profile(dir);
            else if (key == "opt/display.mode") settings_.cycle_display_mode(dir);
            else if (key == "opt/display.vsync") settings_.toggle_vsync();
            if (skirmish_) apply_settings_to_match();
        } else if (b.action == kActAiDiff) settings_.cycle_ai_difficulty(dir);
        else if (b.action == kActSpeed) settings_.cycle_game_speed(dir);
        else if (b.action == kActProfileRace) { settings_.cycle_profile_race(dir); p0_pick_ = settings_.profile_race(); if (p0_pick_ != LobbyRace::Random) slots_[0].race = Race(int(p0_pick_)); }
        else activate(b, float(b.x0 + b.x1) * 0.5f, view_w, view_h);
        return;
    }
    if (edge[0]) fx_.focus_arrow(rects, -1, 0);
    if (edge[1]) fx_.focus_arrow(rects, 1, 0);
    if (edge[2]) fx_.focus_arrow(rects, 0, -1);
    if (edge[3]) fx_.focus_arrow(rects, 0, 1);
    if (id == ScreenId::Lobby || id == ScreenId::Custom) {
        int& scroll = id == ScreenId::Lobby ? map_scroll_ : custom_scroll_;
        const int page = id == ScreenId::Lobby ? lobby_rows_per_page(view_h) : custom_rows_per_page(view_h);
        if (k.page_up) scroll -= page;
        if (k.page_down) scroll += page;
        if (k.home) scroll = 0;
        if (k.end) scroll = int(maps_.size());
        if (id == ScreenId::Lobby) clamp_map_scroll(view_h); else clamp_custom_scroll(view_h);
    }
    if ((k.space || k.enter) && fx_.focus >= 0 && fx_.focus < int(bs.size())) {
        const Button b = bs[std::size_t(fx_.focus)];
        activate(b, float(b.x0 + b.x1) * 0.5f, view_w, view_h);
    }
}

const IconAtlas::Icon* FrontEnd::map_preview(int index) {
    if (index <= 0 || index >= int(maps_.size())) return nullptr;
    const platform::MapEntry& e = maps_[std::size_t(index)];
    if (e.path.empty()) return nullptr;
    auto it = preview_cache_.find(e.path);
    if (it != preview_cache_.end()) return it->second.ok() ? &it->second : nullptr;
    if (preview_failed_ == e.path) return nullptr;
    IconAtlas::Icon icon;
    try {
        std::vector<std::uint8_t> bytes;
        if (e.casc) bytes = store_.read_by_name(e.path);
        else {
            std::ifstream in(e.path, std::ios::binary);
            bytes.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        }
        formats::Mpq arc = formats::Mpq::open(std::move(bytes));
        if (arc.contains("Minimap.tga")) {
            formats::TgaInfo ti;
            std::vector<std::uint8_t> px = formats::decode_tga(arc.read_file("Minimap.tga"), ti);
            if (ti.width && ti.height && px.size() == std::size_t(ti.width) * ti.height * 4) {
                icon.w = ti.width;
                icon.h = ti.height;
                icon.rgba = std::move(px);
            }
        }
    } catch (const std::exception&) {
        preview_failed_ = e.path;
        return nullptr;
    }
    IconAtlas::Icon& slot = preview_cache_[e.path];
    slot = std::move(icon);
    return slot.ok() ? &slot : nullptr;
}

void FrontEnd::apply_settings_to_match() {
    if (!skirmish_) return;
    skirmish_->set_speed(settings_.game_speed());
    if (MeleeAudio* a = skirmish_->audio())
        for (const ShellSettings::Bus& b : ShellSettings::buses())
            for (const char* cat : b.categories) a->set_category_gain(cat, settings_.bus_gain(b));
    skirmish_->set_status_bar_mode(settings_.unit_status_mode());
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
        if (!options_engine_page_ && options_panel_ >= 0 && std::size_t(options_panel_) < panels.size())
            opts.shown.push_back(panels[std::size_t(options_panel_)]);
    }
    const float shell_ref = float(app::theme::kDesignRefH);
    e.screen = GlueScreen::harvest(store_, paths, ui::Rect{0, 0, shell_ref * float(view_w) / float(view_h), shell_ref}, opts);
    if (e.screen) e.screen->scale(float(view_h) / shell_ref);
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
        if (i == 1) {
            const int diff_x0 = race_x1 + S(theme::kSlotGapX) + S(theme::kColColor) + S(theme::kSlotGapX) +
                                S(theme::kColTeam) + S(theme::kSlotGapX) + S(theme::kColHandicap) + S(theme::kSlotGapX);
            const int diff_x1 = diff_x0 + S(theme::kColAiDiff);
            out.push_back({diff_x0, ry, diff_x1, ry + pd_h, ShellSettings::difficulty_name(settings_.ai_difficulty()),
                           "", kActAiDiff});
            const int sp_x0 = diff_x1 + S(theme::kSlotGapX);
            out.push_back({sp_x0, ry, sp_x0 + S(theme::kColAiBuild), ry + pd_h,
                           ShellSettings::speed_name(settings_.game_speed()), "", kActSpeed});
        }
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

static void stamp_replay_meta(MeleeGame& g, const std::string& map_id, const std::vector<MeleeSlot>& slots, bool any_start_pair) {
    std::vector<std::int32_t> races, teams, ctls;
    for (const MeleeSlot& sl : slots) {
        races.push_back(std::int32_t(sl.race));
        teams.push_back(sl.team);
        ctls.push_back(sl.controller == MeleeSlot::Controller::Human ? 0 : 1);
    }
    g.set_replay_meta(map_id, std::move(races), std::move(teams), std::move(ctls), any_start_pair);
}

static bool arena_sites(const TestMap& m, const std::vector<MeleeSlot>& slots, std::vector<const TestMap::Base*>& out) {
    out.clear();
    if (slots.size() < 2 || slots.size() > 4 || m.players.size() < 2) return false;
    if (slots.size() == 2) {
        out = {&m.players[0].main, &m.players[1].main};
        return true;
    }
    const TestMap::Base* side_sites[2][2] = {{&m.players[0].main, &m.players[0].natural}, {&m.players[1].main, &m.players[1].natural}};
    int used[2] = {0, 0};
    std::vector<std::pair<int, int>> key_side;
    for (std::size_t p = 0; p < slots.size(); ++p) {
        const int key = slots[p].team >= 0 ? slots[p].team : -(int(p) + 2);
        int side = -1;
        for (const auto& ks : key_side)
            if (ks.first == key) side = ks.second;
        if (side < 0) {
            side = key_side.empty() ? 0 : (key_side.size() == 1 ? 1 : (used[1] < used[0] ? 1 : 0));
            key_side.emplace_back(key, side);
        }
        if (used[side] >= 2) side = 1 - side;
        out.push_back(side_sites[side][used[side]++]);
    }
    return true;
}

void FrontEnd::slots_from_log(const OrderLog& log) {
    if (log.player_races.size() < 2) return;
    slots_.clear();
    for (std::size_t p = 0; p < log.player_races.size(); ++p) {
        MeleeSlot sl;
        sl.race = Race(std::max(0, std::min(2, int(log.player_races[p]))));
        sl.team = log.team_of(p);
        sl.controller = log.controller_of(p) == 0 ? MeleeSlot::Controller::Human : MeleeSlot::Controller::AI;
        slots_.push_back(sl);
    }
}

void FrontEnd::capture_players() {
    if (!skirmish_) return;
    const sim::World& w = skirmish_->world();
    const std::int32_t wk = w.winning_team();
    last_players_.clear();
    for (std::size_t p = 0; p < slots_.size(); ++p) {
        PlayerCard c;
        c.race = slots_[p].race;
        c.team = slots_[p].team;
        c.controller = slots_[p].controller;
        c.difficulty = slots_[p].controller == MeleeSlot::Controller::AI ? (slots_[p].difficulty > 0 ? slots_[p].difficulty : settings_.ai_difficulty()) : 0;
        c.won = wk != -1 && w.alliance_key(std::int32_t(p)) == wk;
        last_players_.push_back(c);
    }
}

const char* FrontEnd::lobby_format_name(LobbyFormat f) {
    switch (f) {
        case LobbyFormat::OneVsOne: return "1v1";
        case LobbyFormat::TwoVsTwo: return "2v2";
        case LobbyFormat::ThreeVsThree: return "3v3";
        case LobbyFormat::FourVsFour: return "4v4";
        case LobbyFormat::FreeForAll: return "FFA";
    }
    return "1v1";
}

int FrontEnd::lobby_format_players(LobbyFormat f, int starts) {
    switch (f) {
        case LobbyFormat::OneVsOne: return 2;
        case LobbyFormat::TwoVsTwo: return 4;
        case LobbyFormat::ThreeVsThree: return 6;
        case LobbyFormat::FourVsFour: return 8;
        case LobbyFormat::FreeForAll: return std::max(2, std::min(8, starts));
    }
    return 2;
}

std::vector<FrontEnd::LobbySlot> FrontEnd::lobby_preset(LobbyFormat f, int rows, LobbyRace you, LobbyRace ai, int tier) {
    const int players = lobby_format_players(f, rows);
    std::vector<LobbySlot> out(std::size_t(std::max(rows, players)));
    for (int i = 0; i < int(out.size()); ++i) {
        LobbySlot& s = out[std::size_t(i)];
        if (i >= players) {
            s.controller = MeleeSlot::Controller::Closed;
            s.pick = ai;
            continue;
        }
        s.controller = i == 0 ? MeleeSlot::Controller::Human : MeleeSlot::Controller::AI;
        s.pick = i == 0 ? you : ai;
        s.difficulty = i == 0 ? 0 : tier;
        s.team = (f == LobbyFormat::OneVsOne || f == LobbyFormat::FreeForAll) ? -1 : (i < players / 2 ? 0 : 1);
    }
    return out;
}

bool FrontEnd::set_lobby_format(LobbyFormat f) {
    const int starts = map_start_count(sel_map_);
    if (lobby_format_players(f, starts) > starts) return false;
    lobby_format_ = f;
    lobby_slots_ = lobby_preset(f, starts, settings_.profile_race(), settings_.opponent_race(), settings_.ai_difficulty());
    match_error_.clear();
    return true;
}

int FrontEnd::map_start_count(int index) {
    if (index < 0 || index >= int(maps_.size())) return 2;
    const platform::MapEntry& e = maps_[std::size_t(index)];
    if (e.path.empty()) return 4;
    auto it = map_starts_cache_.find(e.path);
    if (it != map_starts_cache_.end()) return it->second;
    int n = 0;
    try {
        std::vector<std::uint8_t> bytes;
        if (e.casc) bytes = store_.read_by_name(e.path);
        else {
            std::ifstream in(e.path, std::ios::binary);
            bytes.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        }
        formats::Mpq arc = formats::Mpq::open(std::move(bytes));
        if (arc.contains("Objects")) n = int(formats::parse_placed_objects(arc.read_file("Objects")).start_locations.size());
    } catch (const std::exception&) {
    }
    n = n < 2 ? 2 : std::min(n, 8);
    map_starts_cache_[e.path] = n;
    return n;
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
    stamp_replay_meta(*skirmish_, match_map_path_, slots_, match_any_pair_);
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
    slots_from_log(replay_log_pending_);
    match_error_.clear();
    loading_frames_ = 0;
    load_plan_.reset();
    if (replay_log_pending_.map_id.empty() && slots_.size() > 2) {
        clear_score();
        sel_map_ = 0;
        begin_match();
    } else if (replay_log_pending_.map_id.empty()) start_skirmish();
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
    stamp_replay_meta(*skirmish_, match_map_path_, slots_, match_any_pair_);
    if (skirmish_->write_replay(path)) last_recorded_replay_ = path;
}

void FrontEnd::start_sandbox() {
    sandbox_ = std::make_unique<SandboxGame>(cat_, models_, store_, map_);
    router_.replace(ScreenId::Sandbox);
}

void FrontEnd::start_skirmish() {
    clear_score();
    set_two_player_races(Race::Terran, Race::Terran);
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
    for (const MeleeSlot& sl : slots_) {
        const RaceUnits u = race_units(sl.race);
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
            {
                std::unordered_map<std::string, std::vector<std::uint8_t>> ov;
                if (!std::getenv("SC2_NO_MAP_ASSETS")) {
                    try {
                        const formats::Mpq arc = formats::Mpq::open(pending_map_bytes_);
                        for (const std::string& name : arc.list()) {
                            std::string key = name;
                            for (char& c : key) c = c == '\\' ? '/' : char(std::tolower(static_cast<unsigned char>(c)));
                            if (key.compare(0, 7, "assets/") != 0) continue;
                            try { ov.emplace(std::move(key), arc.read_file(name)); } catch (const std::exception&) {}
                        }
                    } catch (const std::exception&) {}
                }
                if (std::getenv("SC2_MAP_ASSETS_TRACE")) std::fprintf(stderr, "[mapassets] %zu files from the map archive\n", ov.size());
                store_.set_overlay(std::move(ov));
            }
            if (!std::getenv("SC2_NO_MAP_CAMPAIGN_UNITS")) {
                try {
                    const formats::Mpq arc = formats::Mpq::open(pending_map_bytes_);
                    const std::vector<std::uint8_t> hb = arc.read_file("DocumentHeader");
                    std::string header(hb.begin(), hb.end());
                    for (char& c : header) c = char(std::tolower(static_cast<unsigned char>(c)));
                    int depth = 0;
                    if (header.find("campaigns/liberty.sc2campaign") != std::string::npos) depth = std::max(depth, 1);
                    if (header.find("campaigns/swarm.sc2campaign") != std::string::npos) depth = std::max(depth, 2);
                    if (header.find("campaigns/void.sc2campaign") != std::string::npos) depth = std::max(depth, 3);
                    static int loaded_depth = 0;
                    if (depth > loaded_depth) {
                        static const char* const kChain[] = {"campaigns/liberty.sc2campaign/base.sc2data",
                                                             "campaigns/swarm.sc2campaign/base.sc2data",
                                                             "campaigns/void.sc2campaign/base.sc2data"};
                        std::vector<std::string> roots(kChain + loaded_depth, kChain + depth);
                        cat_.load_missing_only(store_, roots);
                        loaded_depth = depth;
                        if (std::getenv("SC2_PLACED_DROP_TRACE"))
                            std::fprintf(stderr, "[map-campaign] dependency depth %d: %zu campaign root(s) added (missing ids only)\n", depth, roots.size());
                    }
                } catch (const std::exception&) {
                }
            }
            real_map_ = std::make_unique<formats::LoadedMap>(
                formats::load_map_terrain(std::move(pending_map_bytes_)));
            pending_map_bytes_.clear();
            models_.set_tileset(store_, real_map_->tileset);
            app::stamp_map_doodads(cat_, *real_map_);
            app::stamp_map_units(cat_, *real_map_);
            if (!std::getenv("SC2_NO_DOODAD_MODEL_SCALE")) {
                std::map<std::string, std::array<float, 3>> model_scale;
                for (formats::PlacedDoodad& d : real_map_->objects.doodads) {
                    auto it = model_scale.find(d.type);
                    if (it == model_scale.end()) it = model_scale.emplace(d.type, data::resolve_doodad_model_scale(cat_, d.type)).first;
                    d.sx *= it->second[0];
                    d.sy *= it->second[1];
                    d.sz *= it->second[2];
                }
            }
            current_env_ = data::resolve_map_environment(cat_, real_map_->tileset);
            if (const std::vector<std::uint8_t>* lx = real_map_->game_data_file("LightData.xml")) data::overlay_map_light(current_env_, *lx);
            if (const std::vector<std::uint8_t>* tx = real_map_->game_data_file("TerrainData.xml")) data::overlay_map_terrain(current_env_, *tx, real_map_->tileset);
            current_env_.params["EngineZOffset"] = formats::engine_z_offset(*real_map_);
            if (std::getenv("SC2_ENV_TRACE"))
                std::fprintf(stderr, "env: tileset %s -> light %s (%s) found=%d exposure=%.3f white=%.3f ambient=%.3f,%.3f,%.3f key=%d params=%zu\n",
                             real_map_->tileset.c_str(), current_env_.light_id.c_str(), current_env_.resolution.c_str(),
                             int(current_env_.found), current_env_.hdr_exposure, current_env_.hdr_white_point,
                             current_env_.ambient_color[0], current_env_.ambient_color[1], current_env_.ambient_color[2],
                             int(current_env_.key.present), current_env_.params.size());
        } else if (phase == 2) {
            real_cliffs_ = app::place_cliffs(store_, *real_map_);
            app::apply_cliff_vertex_shade(real_cliffs_, *real_map_);
        } else {
            const bool vertex_ground = !real_map_->cliff.empty();
            const bool cliff_ground = vertex_ground && !real_cliffs_.instances.empty() && !std::getenv("SC2_NO_CLIFF_GROUND");
            const std::vector<std::uint8_t> cliff_cells = cliff_ground ? app::cliff_ground_cells(real_cliffs_, *real_map_) : std::vector<std::uint8_t>{};
            real_ground_ = vertex_ground ? formats::build_map_mesh_vertex(*real_map_, 1.0f, cliff_ground ? &cliff_cells : nullptr)
                                         : formats::build_map_mesh_stepped(*real_map_, 1.0f, 128, true);
            if (vertex_ground) app::flatten_adjusted_terrain(real_ground_, store_, models_, *real_map_);
            if (cliff_ground) app::append_cliff_ground(real_ground_, real_cliffs_, *real_map_);
            if (vertex_ground) append_terrain_material_doodads(real_ground_, store_, models_, *real_map_);
            if (texture_cb_) texture_cb_(real_ground_, *real_map_);
            const std::vector<std::uint8_t>* map_water_xml = real_map_->game_data_file("WaterData.xml");
            const auto water_heights = water_type_heights(store_, map_water_xml);
            real_water_ = formats::build_map_water(*real_map_, 1.0f, 0.5f, &water_heights);
            real_water_rgba_ = {1.0f, 1.0f, 1.0f, 1.0f};
            real_roads_ = !std::getenv("SC2_ROAD_TERRAIN_SPEC")
                              ? build_map_roads(store_, *real_map_)
                              : build_map_roads(store_, *real_map_, float(current_env_.param("TerrainSpecularExp", 100.0)),
                                                float(current_env_.param("TerrainHDRSpecular", 1.0) / std::max(current_env_.param("HDRSpecMultiplier", 1.0), 1e-3)));
            const double proj_spec_mult = current_env_.param("HDRSpecMultiplier", 1.0);
            append_ground_overlay(real_roads_, build_map_projectors(store_, models_, *real_map_, float(current_env_.param("TerrainSpecularExp", 100.0)),
                                                                    float(current_env_.param("TerrainHDRSpecular", 1.0) / (proj_spec_mult > 0.0 ? proj_spec_mult : 1.0))));
            if (!std::getenv("SC2_ROAD_OVERHANG")) {
                const float mw = float(real_map_->cell_width()), mh = float(real_map_->cell_height());
                formats::Model& rm = real_roads_;
                for (const formats::M3Region& rg : rm.regions)
                    for (std::uint32_t t = rg.first_index; t + 2 < rg.first_index + rg.num_indices && t + 2 < rm.indices.size(); t += 3) {
                        float cx = 0.0f, cy = 0.0f;
                        bool ok = true;
                        for (int k = 0; k < 3; ++k) {
                            const std::size_t v = std::size_t(rg.first_vertex) + rm.indices[t + std::uint32_t(k)];
                            if (v * 3 + 1 >= rm.positions.size()) { ok = false; break; }
                            cx += rm.positions[v * 3];
                            cy += rm.positions[v * 3 + 1];
                        }
                        if (!ok) continue;
                        cx /= 3.0f;
                        cy /= 3.0f;
                        if (cx < 0.0f || cy < 0.0f || cx > mw || cy > mh) rm.indices[t + 1] = rm.indices[t + 2] = rm.indices[t];
                    }
            }
            if (std::getenv("SC2_ENV_TRACE")) {
                auto zr = [](const formats::Model& m, float& lo, float& hi) {
                    lo = 1e9f; hi = -1e9f;
                    for (std::size_t v = 2; v < m.positions.size(); v += 3) { lo = std::min(lo, m.positions[v]); hi = std::max(hi, m.positions[v]); }
                };
                float gl, gh, wl, wh;
                zr(real_ground_, gl, gh); zr(real_water_, wl, wh);
                std::fprintf(stderr, "env: ground z %.2f..%.2f  water z %.2f..%.2f (%zu planes, type %s)\n", gl, gh, wl, wh,
                             real_map_->water.planes.size(), real_map_->water.planes.empty() ? "-" : real_map_->water.planes.front().type.c_str());
            }
            if (!real_map_->water.planes.empty()) {
                const auto mats = water_type_materials(store_, map_water_xml);
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
        fx_.toast(match_error_, true);
        real_map_.reset();
        real_cliffs_ = CliffField{};
        real_doodads_.clear();
        real_fluff_.clear();
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
    {
        std::array<int, 16> none;
        none.fill(-1);
        set_map_team_slots(none);
    }
    const formats::Model* ground = nullptr;
    float map_w = 0, map_h = 0;
    const sim::NavGrid* nav = nullptr;
    const sim::TerrainMap* placement = nullptr;
    float playable_x0 = -1.0f, playable_y0 = -1.0f, playable_x1 = -1.0f, playable_y1 = -1.0f;

    if (synthetic) {
        match_any_pair_ = false;
        data::Production prod;
        prod.build(cat_);
        std::vector<const TestMap::Base*> sites;
        if (!arena_sites(map_, slots_, sites)) {
            match_error_ = "the built-in arena seats 2 to 4 players (its two mains and two naturals); this setup has " +
                           std::to_string(slots_.size());
            router_.replace(ScreenId::Lobby);
            return false;
        }
        for (std::size_t p = 0; p < slots_.size(); ++p) {
            const MeleeProtos pr = race_protos(cat_, slots_[p].race);
            const TestMap::Base& base = *sites[p];
            MeleePlayerSetup ps;
            ps.team = slots_[p].team;
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
            apply_race_costs(ps, cat_, slots_[p].race, prod);
            setup.players.push_back(ps);
        }
        ground = &map_.ground;
        map_w = map_.width;
        map_h = map_.height;
        nav = &map_.nav;
    } else {
        for (int phase = 0; phase < 4; ++phase) {
            if (!build_real_map_phase(phase)) {
                router_.replace(ScreenId::Lobby);
                return false;
            }
            static const char* const kPhase[] = {"map: phase 0 (archive, terrain)", "map: phase 1", "map: phase 2", "map: phase 3"};
            render::mem_mark(kPhase[phase]);
        }
        const std::uint64_t start_seed = replay_pending_ ? replay_log_pending_.seed : (match_seed_ ? match_seed_ : MeleeGameConfig{}.seed);
        const bool any_pair = replay_pending_ ? replay_log_pending_.any_start_pair : true;
        match_any_pair_ = any_pair && slots_.size() == 2 && map_melee_any_pair_rule(*real_map_);
        if (slots_.size() == 2 && slots_[0].team == -1 && slots_[1].team == -1) {
            setup = melee_setup_for_map(*real_map_, cat_, slots_[0].race, slots_[1].race, -1, 8, start_seed, any_pair);
        } else {
            try {
                setup = melee_setup_for_map(*real_map_, cat_, slots_, -1, 8, start_seed, any_pair);
            } catch (const std::exception& e) {
                match_error_ = std::string("this map cannot seat the setup: ") + e.what();
                router_.replace(ScreenId::Lobby);
                return false;
            }
        }
        ground = &real_ground_;
        map_w = float(real_map_->width);
        map_h = float(real_map_->height);
        nav = &real_map_->nav;
        placement = &real_map_->placement;
        if (real_map_->info.has_players) {
            std::array<int, 16> slots;
            slots.fill(-1);
            for (std::size_t pid = 1; pid < 16; ++pid) slots[pid - 1] = int(real_map_->info.player_color[pid]);
            set_map_team_slots(slots);
        }
        if (real_map_->info.has_playable_area()) {
            playable_x0 = float(real_map_->info.playable_x0);
            playable_y0 = float(real_map_->info.playable_y0);
            playable_x1 = float(real_map_->info.playable_x1);
            playable_y1 = float(real_map_->info.playable_y1);
        }
    }

    for (const MeleeSlot& sl : slots_) {
        const RaceUnits u = race_units(sl.race);
        for (const char* tok : {u.hall, u.worker, u.producer, u.supply, u.army})
            models_.ensure(store_, tok);
    }
    for (const sim::Unit& n : setup.neutrals)
        if (!n.catalog_id.empty()) models_.ensure(store_, n.catalog_id);

    MeleeGameConfig cfg;
    cfg.human_player = melee_human_player(slots_);
    if (replay_pending_) cfg.seed = replay_log_pending_.seed;
    else if (match_seed_) cfg.seed = match_seed_;
    if (!replay_pending_) cfg.ai_difficulty = settings_.ai_difficulty();
    if (!replay_pending_) {
        const std::vector<int> rungs = melee_slot_difficulties(slots_);
        for (int r : rungs)
            if (r > 0) cfg.slot_difficulty = rungs;
    }
    render::mem_mark("match: setup chosen, race models");
    skirmish_ = std::make_unique<MeleeGame>(cfg);
    render::mem_mark("match: MeleeGame constructed");
    if (!replay_pending_) apply_settings_to_match();
    skirmish_->set_ground_fog(ground_fog_);
    skirmish_->set_reveal_map(reveal_map_);
    if (real_map_ && !real_map_->info.fog_type.empty()) skirmish_->set_fog_type(real_map_->info.fog_type);
    if (double_click_ms_ > 0.0) skirmish_->set_double_click_ms(double_click_ms_);
    if (console_renderer_) skirmish_->set_console_renderer(console_renderer_);
    if (portrait_renderer_) skirmish_->set_portrait_renderer(portrait_renderer_);
    skirmish_->set_ui_storage(&store_);
    skirmish_->set_build_verb(sim::BuildVerb::Committed);
    skirmish_->register_creep_masks(cat_);
    skirmish_->set_creep_diffuse(render::resolve_creep_diffuse(store_));
    if (real_map_ && !std::getenv("SC2_CREEP_CORE_TEXTURE"))
        if (auto v = cat_.scalar("CTerrain", real_map_->tileset, "CreepBaseTexture"); v && !v->empty()) {
            std::string rel = *v;
            for (char& c : rel)
                if (c == '\\') c = '/';
            skirmish_->set_creep_diffuse(rel);
        }
    if (real_map_) {
        skirmish_->set_tier_heights(formats::tier_heights(*real_map_));
        skirmish_->set_air_floor(formats::air_floor_z(*real_map_));
        std::uint32_t gw = 0, gh = 0;
        std::vector<float> gh_grid = !real_map_->cliff.empty() ? formats::bake_ground_heights_vertex(*real_map_, gw, gh)
                                                                     : formats::bake_ground_heights(*real_map_, gw, gh);
        if (!gh_grid.empty()) skirmish_->set_ground_heights(std::move(gh_grid), gw, gh);
        if (!real_map_->cliff.empty()) {
            const std::vector<float> vth = formats::tier_heights(*real_map_);
            const float voff = formats::engine_z_offset(*real_map_);
            const std::uint32_t vw = real_map_->cliff.width + 1, vh = real_map_->cliff.height + 1;
            std::vector<float> vgrid(std::size_t(vw) * vh, 0.0f);
            for (std::uint32_t r = 0; r < vh; ++r)
                for (std::uint32_t c = 0; c < vw; ++c) vgrid[std::size_t(r) * vw + c] = formats::vertex_ground_z(*real_map_, vth, voff, c, r);
            if (const char* gt = std::getenv("SC2_GROUND_TRACE")) {
                int bx0 = 0, bx1 = 0, by0 = 0, by1 = 0;
                if (std::sscanf(gt, "%d,%d,%d,%d", &bx0, &bx1, &by0, &by1) == 4) {
                    std::map<std::pair<int, int>, float> zs;
                    for (std::size_t i = 0; i + 2 < real_ground_.positions.size(); i += 3) {
                        const float px = real_ground_.positions[i], py = real_ground_.positions[i + 1];
                        const int ix = int(std::lround(px)), iy = int(std::lround(py));
                        if (std::fabs(px - float(ix)) > 1e-3f || std::fabs(py - float(iy)) > 1e-3f) continue;
                        if (ix < bx0 || ix > bx1 || iy < by0 || iy > by1) continue;
                        zs.emplace(std::make_pair(ix, iy), real_ground_.positions[i + 2] + voff);
                    }
                    float wmin = 1e9f, wmax = -1e9f;
                    for (std::size_t i = 2; i < real_water_.positions.size(); i += 3) {
                        wmin = std::min(wmin, real_water_.positions[i] + voff);
                        wmax = std::max(wmax, real_water_.positions[i] + voff);
                    }
                    std::fprintf(stderr, "[ground] water plane z %.3f .. %.3f (offset %.3f)\n", double(wmin), double(wmax), double(voff));
                    for (int y = by1; y >= by0; --y) {
                        std::fprintf(stderr, "[ground] %3d", y);
                        for (int x = bx0; x <= bx1; ++x) {
                            auto it = zs.find({x, y});
                            std::fprintf(stderr, " %5.2f", it == zs.end() ? -99.0 : double(it->second));
                        }
                        std::fprintf(stderr, "\n");
                    }
                }
            }
            skirmish_->set_vertex_heights(std::move(vgrid), vw, vh);
        }
        skirmish_->set_minimap_image(real_map_->minimap_rgba, real_map_->minimap_w,
                                     real_map_->minimap_h);
    }
    skirmish_->setup(setup, &models_, ground, map_w, map_h, nav, placement, playable_x0,
                     playable_y0, playable_x1, playable_y1);
    render::mem_mark("match: MeleeGame::setup (world, nav, actors)");
    if (real_map_ && real_map_->scripted_camera.found && !std::getenv("SC2_NO_MAP_CAMERA")) {
        const formats::ScriptedCamera& sc = real_map_->scripted_camera;
        render::CameraControl& cc = skirmish_->camera_control();
        cc.distance = sc.distance;
        cc.max_distance = std::max(cc.max_distance, sc.distance);
        cc.min_distance = std::min(cc.min_distance, sc.distance);
        cc.pitch = sc.pitch_deg * render::kDegToRad;
        cc.fovy = sc.fov_deg * render::kDegToRad;
        cc.yaw = -1.5707963f + (sc.yaw_deg - 180.0f) * render::kDegToRad;
        cc.height_offset = 0.0f;
    }
    if (real_map_) skirmish_->set_water(&real_water_, real_water_rgba_);
    if (real_map_)
        if (const std::vector<std::uint8_t>* wx = real_map_->game_data_file("WaterData.xml")) skirmish_->set_map_water_xml(*wx);
    if (real_map_) skirmish_->set_roads(&real_roads_);
    if (real_map_) {
        real_doodads_ = real_map_->objects.doodads;
        const auto& cm = real_map_->cliff;
        const std::vector<float> th = formats::tier_heights(*real_map_);
        const float z_off = formats::engine_z_offset(*real_map_);
        std::map<std::string, float> actor_scale;
        const bool no_actor_scale = std::getenv("SC2_DOODAD_ACTOR_SCALE") == nullptr;
        for (formats::PlacedDoodad& d : real_doodads_) {
            if (!no_actor_scale) {
                auto it = actor_scale.find(d.type);
                if (it == actor_scale.end()) {
                    float s = 1.0f;
                    if (cat_.find("CActorDoodad", d.type))
                        for (const core::XmlNode& n : cat_.resolved("CActorDoodad", d.type)) {
                            if (n.name != "RandomScaleRange") continue;
                            const std::string* v = n.attr("value");
                            float a = 1.0f, b = 1.0f;
                            if (v && std::sscanf(v->c_str(), "%f,%f", &a, &b) == 2) s = (a == b && a > 0.0f) ? a : 1.0f;
                        }
                    it = actor_scale.emplace(d.type, s).first;
                }
                d.sx *= it->second;
                d.sy *= it->second;
                d.sz *= it->second;
            }
            const int cx = int(d.x), cy = int(d.y);
            if (!th.empty() && !cm.empty() && cx >= 0 && cy >= 0 && std::uint32_t(cx) < cm.width &&
                std::uint32_t(cy) < cm.height) {
                const std::size_t L = std::size_t(cm.levels[std::size_t(cy) * cm.width + cx]);
                static const char* kZAuthored = std::getenv("SC2_DOODAD_Z_AUTHORED");
                const bool z_authored = kZAuthored && d.type.compare(0, std::strlen(kZAuthored), kZAuthored) == 0;
                if (z_authored) {
                    static int shown = 0;
                    if (shown++ < 6) std::fprintf(stderr, "[doodadz] %s at %.2f %.2f authored %.3f offset %.3f tier %.3f\n", d.type.c_str(), double(d.x), double(d.y), double(d.z), double(z_off), double(th[L < th.size() ? L : th.size() - 1]));
                }
                d.z = (d.height_absolute || z_authored) ? d.z - z_off : th[L < th.size() ? L : th.size() - 1];
            } else {
                d.z = 0.0f;
            }
        }
        if (std::getenv("SC2_SHORELINE") && !std::getenv("SC2_NO_SHORELINE")) {
            const std::vector<std::uint8_t>* sw = real_map_->game_data_file("WaterData.xml");
            static const float kShoreSpacing = std::getenv("SC2_SHORELINE_SPACING") ? float(std::atof(std::getenv("SC2_SHORELINE_SPACING"))) : 2.0f;
            const auto shore = shoreline_placements(*real_map_, water_type_heights(store_, sw), store_, sw, kShoreSpacing);
            real_doodads_.insert(real_doodads_.end(), shore.begin(), shore.end());
            if (std::getenv("SC2_SHORELINE_TRACE")) {
                std::fprintf(stderr, "[shoreline] %zu instances\n", shore.size());
                for (std::size_t k = 0; k < shore.size() && k < 12; ++k)
                    std::fprintf(stderr, "[shoreline]   %s at (%.2f, %.2f, z %.2f) facing %.2f\n", shore[k].type.c_str(), double(shore[k].x), double(shore[k].y),
                                 double(shore[k].z), double(shore[k].rotation));
            }
        }
        skirmish_->set_doodads(&real_doodads_);
        real_fluff_.clear();
        if (!real_map_->fluff.empty() && !std::getenv("SC2_NO_FLUFF")) {
            struct FluffRow { std::string model; float prob = 0.0f; bool random_rotation = false; int variations = 1; float smin = 1.0f, smax = 1.0f; };
            std::vector<std::vector<FluffRow>> rows(real_map_->tileset_textures.size());
            for (std::size_t s = 0; s < rows.size(); ++s) {
                const std::string& tex = real_map_->tileset_textures[s];
                if (tex.empty() || !cat_.find("CTerrainTex", tex)) continue;
                for (const core::XmlNode& n : cat_.resolved("CTerrainTex", tex)) {
                    if (n.name != "DoodadEntry") continue;
                    FluffRow r;
                    for (const core::XmlNode& c : n.children) {
                        const std::string v = c.attr_or("value", "");
                        if (c.name == "Model") r.model = v;
                        else if (c.name == "Probability") r.prob = std::strtof(v.c_str(), nullptr);
                        else if (c.name == "RandomRotation") r.random_rotation = v != "0";
                    }
                    if (r.model.empty() || r.prob <= 0.0f || !cat_.find("CModelFoliage", r.model)) continue;
                    for (const core::XmlNode& f : cat_.resolved("CModelFoliage", r.model)) {
                        const std::string v = f.attr_or("value", "");
                        if (f.name == "VariationCount") r.variations = std::max(1, std::atoi(v.c_str()));
                        else if (f.name == "ScaleMin") r.smin = std::strtof(v.c_str(), nullptr);
                        else if (f.name == "ScaleMax") r.smax = std::strtof(v.c_str(), nullptr);
                    }
                    rows[s].push_back(std::move(r));
                }
                if (std::getenv("SC2_ENV_TRACE")) {
                    std::fprintf(stderr, "fluff: slot %zu '%s' found %d rows %zu\n", s, tex.c_str(), int(cat_.find("CTerrainTex", tex) != nullptr), rows[s].size());
                    for (const FluffRow& r : rows[s])
                        std::fprintf(stderr, "fluff:   row '%s' probability %.3f variations %d scale %.3f .. %.3f\n", r.model.c_str(), double(r.prob), r.variations,
                                     double(r.smin), double(r.smax));
                }
            }
            const auto hash01 = [](std::uint32_t i, std::uint32_t salt) {
                std::uint32_t h = i * 0x9E3779B1u ^ (salt * 0x85EBCA6Bu);
                h ^= h >> 15; h *= 0x2C1B3C6Du; h ^= h >> 12; h *= 0x297A2D39u; h ^= h >> 15;
                return float(h & 0xFFFFFFu) / float(0x1000000);
            };
            real_fluff_.reserve(real_map_->fluff.size());
            for (std::size_t i = 0; i < real_map_->fluff.size(); ++i) {
                const formats::LoadedMap::FluffDoodad& f = real_map_->fluff[i];
                if (std::size_t(f.slot) >= rows.size() || rows[f.slot].empty()) continue;
                const std::vector<FluffRow>& rs = rows[f.slot];
                static const bool kWeights = std::getenv("SC2_FLUFF_WEIGHTS") != nullptr;
                const FluffRow* row = nullptr;
                if (kWeights) {
                    float total = 0.0f;
                    for (const FluffRow& r : rs) total += r.prob;
                    float pick = hash01(std::uint32_t(i), 1) * total;
                    row = &rs.back();
                    for (const FluffRow& r : rs) {
                        if (pick < r.prob) { row = &r; break; }
                        pick -= r.prob;
                    }
                } else {
                    for (std::size_t k = 0; k < rs.size(); ++k)
                        if (hash01(std::uint32_t(i), 16 + std::uint32_t(k)) < rs[k].prob) { row = &rs[k]; break; }
                }
                if (!row) continue;
                formats::PlacedDoodad d;
                d.type = row->model;
                d.x = f.x;
                d.y = f.y;
                d.z = formats::vertex_ground_bilinear(*real_map_, th, z_off, f.x, f.y);
                d.rotation = row->random_rotation ? hash01(std::uint32_t(i), 2) * 6.2831853f : 0.0f;
                d.variation = std::min(row->variations - 1, int(hash01(std::uint32_t(i), 3) * float(row->variations)));
                d.sx = d.sy = d.sz = row->smin + (row->smax - row->smin) * hash01(std::uint32_t(i), 4);
                real_fluff_.push_back(std::move(d));
            }
        }
        if (std::getenv("SC2_ENV_TRACE"))
            std::fprintf(stderr, "fluff: %zu records, %zu texture slots, %zu resolved\n", real_map_->fluff.size(), real_map_->tileset_textures.size(), real_fluff_.size());
        skirmish_->set_fluff(&real_fluff_);
        if (std::getenv("SC2_ENV_TRACE"))
            std::fprintf(stderr, "bounds: map %u x %u cells, camera l %d b %d r %d t %d (has %d)\n", real_map_->cell_width(), real_map_->cell_height(),
                         int(real_map_->info.camera_left), int(real_map_->info.camera_bottom), int(real_map_->info.camera_right), int(real_map_->info.camera_top),
                         int(real_map_->info.has_camera_bounds));
        if (real_map_->info.has_camera_bounds)
            skirmish_->set_camera_bounds_x(float(real_map_->info.camera_left), float(real_map_->info.camera_right));
        skirmish_->set_cliffs(&real_cliffs_.instances);
    }
    if (!research_up_) {
        research_up_ = std::make_unique<data::UpgradeCatalog>();
        try {
            research_up_->load_melee(store_);
        } catch (const std::exception&) {
        }
    }
    std::vector<Race> races;
    for (const MeleeSlot& sl : slots_) races.push_back(sl.race);
    render::mem_mark("match: doodads, cliffs, research catalog");
    apply_shipping_wiring(*skirmish_, cat_, research_up_.get(), &setup, &races);
    render::mem_mark("match: shipping wiring");
    if (portrait_full_rate_) skirmish_->set_portrait_hz(0.0f);
    for (std::size_t p = 0; p < slots_.size(); ++p) {
        const RaceUnits u = race_units(slots_[p].race);
        skirmish_->set_spawn_models(int(p), models_.model_for(u.worker), models_.model_for(u.army),
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
    cursor_x_ = in.mouse_x;
    cursor_y_ = in.mouse_y;
    if (!boot_screen_request().empty()) {
        std::string name;
        name.swap(boot_screen_request());
        if (!enter_screen(name)) std::fprintf(stderr, "crafter-sc: --screen %s: not a screen the client can open on\n", name.c_str());
    }
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
            remember_last_match();
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
            if (!skirmish_->replay_active()) settings_.record_match(last_outcome_ == MatchResult::Outcome::Victory);
            remember_last_match();
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
        loading_ms_ = loading_frames_ < 1 && !load_plan_.planned ? 0.0f : loading_ms_ + dt_ms;
        if (last_drawn_ != ScreenId::Loading) {
            fx_.on_screen_change();
            ui_.reset_screen();
            last_drawn_ = ScreenId::Loading;
        }
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

    mouse_x_ = in.mouse_x;
    mouse_y_ = in.mouse_y;
    left_down_ = in.left_down;
    const bool click = in.left_pressed && !prev_left_ && in.view_w && in.view_h;
    if (router_.top() != last_drawn_) {
        fx_.on_screen_change();
        ui_.reset_screen();
        if (router_.top() == ScreenId::Lobby) {
            if (!shell_v1()) p1_pick_ = settings_.opponent_race();
            p0_pick_ = settings_.profile_race();
            if (p0_pick_ != LobbyRace::Random) slots_[0].race = Race(int(p0_pick_));
        }
        if (router_.top() != ScreenId::Profile) profile_editing_ = false;
        last_drawn_ = router_.top();
    }
    {
        bool esc = false;
        if (run_shell_screen(shell::Pass::Input, in.view_w, in.view_h, &in, nullptr, dt_ms, nullptr, &esc)) {
            fx_.tick(dt_ms, ui_.hot);
            if (esc && !router_.cancel() && router_.top() != ScreenId::GlueRoot && router_.top() != ScreenId::Lobby) {
                prev_left_ = in.left_pressed;
                return false;
            }
            if (router_.top() == ScreenId::Sandbox && !sandbox_) start_sandbox();
            prev_left_ = in.left_pressed;
            if (router_.top() == ScreenId::Boot) return false;
            scene.clear();
            cam = menu_ctl_.camera();
            return true;
        }
    }
    std::vector<Button> bs = in.view_w && in.view_h ? screen_buttons(in.view_w, in.view_h) : std::vector<Button>{};
    int hovered = -1;
    for (std::size_t n = bs.size(); n-- > 0;)
        if (bs[n].x1 > bs[n].x0 && bs[n].y1 > bs[n].y0 && bs[n].hit(in.mouse_x, in.mouse_y)) { hovered = int(n); break; }
    fx_.tick(dt_ms, hovered >= 0 ? (bs[std::size_t(hovered)].trigger.empty() ? bs[std::size_t(hovered)].label : bs[std::size_t(hovered)].trigger)
                                 : std::string());
    if (hovered >= 0 && (in.mouse_dx != 0.0f || in.mouse_dy != 0.0f)) fx_.focus = hovered;

    if (in.view_h && (router_.top() == ScreenId::Lobby || router_.top() == ScreenId::Custom)) {
        int& scroll = router_.top() == ScreenId::Lobby ? map_scroll_ : custom_scroll_;
        if (in.wheel != 0.0f) scroll -= int(in.wheel > 0.0f ? 1 : -1) * 3;
        if (router_.top() == ScreenId::Lobby) clamp_map_scroll(in.view_h); else clamp_custom_scroll(in.view_h);
    }

    if (in.quit) {
        if (router_.top() == ScreenId::GlueRoot && quit_confirm_) quit_confirm_ = false;
        else if (router_.top() == ScreenId::Profile && profile_editing_) profile_editing_ = false;
        else if (!router_.cancel() && router_.top() != ScreenId::GlueRoot && router_.top() != ScreenId::Lobby) {
            prev_left_ = in.left_pressed;
            return false;
        }
    } else if (click) {
        if (hovered >= 0) {
            const Button b = bs[std::size_t(hovered)];
            fx_.focus = hovered;
            activate(b, in.mouse_x, in.view_w, in.view_h);
        } else if (router_.top() == ScreenId::Profile) {
            profile_editing_ = false;
        }
    } else if (in.view_w && in.view_h) {
        shell_keyboard(in, in.view_w, in.view_h);
    }
    if (router_.top() == ScreenId::Sandbox && !sandbox_) start_sandbox();
    if (router_.top() == ScreenId::Boot) { prev_left_ = in.left_pressed; return false; }
    if (router_.top() == ScreenId::Loading) { loading_frames_ = 0; load_plan_.reset(); }
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
    if (!load_face("MenuBarButton", overlay_font_bold_, face_path_[2])) {
        for (const char* mod : kMods) {
            const std::string path = std::string(mod) + "UI/Fonts/Eurostile-Bol.otf";
            try {
                const auto bytes = store_.read_by_name(path);
                if (overlay_font_bold_.load(bytes)) { face_path_[2] = path; break; }
            } catch (const std::exception&) {
            }
        }
    }
    if (!overlay_font_bold_.valid() && overlay_font_.valid() && !face_path_[0].empty()) {
        try {
            overlay_font_bold_.load(store_.read_by_name(face_path_[0]));
        } catch (const std::exception&) {
        }
    }
    if (overlay_font_.valid()) draw::set_active_font(&overlay_font_);
}

std::vector<std::pair<std::string, ui::Rect>> FrontEnd::screen_controls(std::uint32_t view_w,
                                                                       std::uint32_t view_h) {
    std::vector<std::pair<std::string, ui::Rect>> out;
    {
        std::vector<shell::Control> cs;
        if (run_shell_screen(shell::Pass::Collect, view_w, view_h, nullptr, nullptr, 0.0f, &cs, nullptr)) {
            for (const shell::Control& c : cs)
                if (!c.region)
                    out.push_back({c.trigger.empty() || !c.enabled ? c.id : c.trigger,
                                   ui::Rect{float(c.rect.x0), float(c.rect.y0), float(c.rect.w()), float(c.rect.h())}});
            return out;
        }
    }
    for (const Button& b : screen_buttons(view_w, view_h))
        out.push_back({b.trigger.empty() ? b.label : b.trigger,
                       ui::Rect{float(b.x0), float(b.y0), float(b.x1 - b.x0), float(b.y1 - b.y0)}});
    return out;
}

bool FrontEnd::current_screen_is_authored(std::uint32_t view_w, std::uint32_t view_h) {
    const ScreenId id = router_.top();
    if (shell_v2_screen()) return false;
    if (id == ScreenId::GlueRoot) return real_menu(view_w, view_h) != nullptr;
    if (id == ScreenId::Lobby || id == ScreenId::VersusAI || id == ScreenId::Custom || id == ScreenId::Profile) return false;
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
        const int sy = ty + draw::text_h_em(type_px(b, theme::kTypeLoadTitle)) + b.s(8);
        text_box(b, 0, sy, int(view_w), sy + draw::text_h_em(type_px(b, theme::kTypeBody)),
                 id == ScreenId::GameResult ? "CONTINUE opens the score screen; CLOSE returns to the main menu (Esc)"
                                            : "SAVE REPLAY keeps the recording; CONTINUE returns to the main menu (Esc)",
                 theme::kTypeBody, theme::kTextMuted);
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
        text_at(b, pad, y + b.s(theme::kRowH) * 2, "Esc goes back", theme::kTypeMicro, theme::kTextMuted, int(view_w) - pad);
        band_footer(b);
        return;
    }
    const std::string src = std::to_string(s->controls().size()) + " authored controls from " +
                            std::to_string(s->documents()) + " of " + std::to_string(docs.size()) +
                            " documents";
    text_at(b, pad, y, src, theme::kTypeCaption, theme::kTextFaint, int(view_w) - pad);
    band_footer(b);
}

namespace {
void fill_looks(std::vector<Ctl>& ctls, const ShellFx& fx, float mouse_x, float mouse_y, bool left_down) {
    const float pulse = fx.pulse();
    for (std::size_t i = 0; i < ctls.size(); ++i) {
        Ctl& c = ctls[i];
        c.look.hover = fx.hover(c.key);
        c.look.focused = fx.focus == int(i);
        c.look.pulse = pulse;
        c.look.pressed = left_down && c.look.hover > 0.0f && mouse_x >= float(c.x0) && mouse_x < float(c.x1) &&
                         mouse_y >= float(c.y0) && mouse_y < float(c.y1);
    }
}
}

void FrontEnd::build_overlay(std::uint32_t view_w, std::uint32_t view_h,
                             std::vector<std::uint8_t>& out) {
    build_overlay_body(view_w, view_h, out);
    cursor_drawn_ = false;
    if (!software_cursor_ || !view_w || !view_h || cursor_x_ < 0.0f || cursor_y_ < 0.0f) return;
    const bool in_match = skirmish_ && (router_.top() == ScreenId::InGame || router_.top() == ScreenId::ReplayControls);
    std::string id = in_match ? skirmish_->cursor_id(skirmish_->cursor_state()) : std::string();
    if (id.empty()) id = "Arrow";
    const auto look = cat_.cursor_look(id);
    if (!look) return;
    const IconAtlas::Icon& ic = cursor_icons_.get(store_, look->texture);
    if (!ic.ok()) return;
    if (out.size() != std::size_t(view_w) * view_h * 4) out.assign(std::size_t(view_w) * view_h * 4, 0);
    const int ox = int(cursor_x_) - look->hotspot_x, oy = int(cursor_y_) - look->hotspot_y;
    for (int y = 0; y < int(ic.h); ++y)
        for (int x = 0; x < int(ic.w); ++x) {
            const int px = ox + x, py = oy + y;
            if (px < 0 || py < 0 || px >= int(view_w) || py >= int(view_h)) continue;
            const std::size_t si = (std::size_t(y) * ic.w + std::size_t(x)) * 4;
            const float sa = float(ic.rgba[si + 3]) / 255.0f;
            if (sa <= 0.0f) continue;
            const std::size_t di = (std::size_t(py) * view_w + std::size_t(px)) * 4;
            const float da = float(out[di + 3]) / 255.0f;
            const float oa = sa + da * (1.0f - sa);
            for (int c = 0; c < 3; ++c) {
                const float v = (float(ic.rgba[si + c]) * sa + float(out[di + c]) * da * (1.0f - sa)) / oa;
                out[di + c] = std::uint8_t(v + 0.5f);
            }
            out[di + 3] = std::uint8_t(oa * 255.0f + 0.5f);
        }
    cursor_drawn_ = true;
}

void FrontEnd::build_overlay_body(std::uint32_t view_w, std::uint32_t view_h,
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
    if (run_shell_screen(shell::Pass::Paint, view_w, view_h, nullptr, &out, 0.0f, nullptr, nullptr)) return;
    Board b{out, view_w, view_h};
    b.face[0] = &overlay_font_;
    b.face[1] = &overlay_font_ext_;
    b.style_rgb = &style_rgb_;

    const float lit = fx_.fade();
    switch (screen) {
        case ScreenId::GlueRoot:
        case ScreenId::Home:
            background(b, mix_rgb(theme::kGround, theme::kVignetteCore, lit), theme::kGround);
            break;
        case ScreenId::Loading:
            background(b, mix_rgb(theme::kGround, theme::kVignetteLoad, lit), theme::kGround);
            break;
        case ScreenId::PauseMenu:
            background(b, mix_rgb(theme::kGroundDeep, theme::kVignettePause, lit), theme::kGroundDeep);
            break;
        default:
            background(b, mix_rgb(theme::kGround, theme::kVignetteLobby, lit), theme::kGround);
            break;
    }

    std::vector<Ctl> ctls;
    const std::vector<Button> bs = screen_buttons(view_w, view_h);
    for (const Button& bt : bs) {
        Ctl k;
        k.x0 = bt.x0;
        k.y0 = bt.y0;
        k.x1 = bt.x1;
        k.y1 = bt.y1;
        k.label = bt.label;
        k.key = bt.trigger.empty() ? bt.label : bt.trigger;
        k.action = bt.action;
        k.live = bt.action != 0 || router_.edge_for(trigger_for_control(screen, k.key)) != nullptr;
        if (screen == ScreenId::Options && k.key.size() > 8 && k.key.compare(k.key.size() - 8, 8, "CheckBox") == 0) {
            k.live = true;
            k.selected = option_checked(k.key);
        }
        if (screen == ScreenId::Options && k.key.size() > 6 && k.key.compare(k.key.size() - 6, 6, "Button") == 0 &&
            k.action == 0)
            k.live = true;
        ctls.push_back(std::move(k));
    }
    fill_looks(ctls, fx_, mouse_x_, mouse_y_, left_down_);

    if (screen == ScreenId::GlueRoot) {
        if (quit_confirm_) {
            std::vector<Ctl> under;
            for (const Button& bt : menu_buttons(view_w, view_h)) {
                Ctl k;
                k.x0 = bt.x0; k.y0 = bt.y0; k.x1 = bt.x1; k.y1 = bt.y1;
                k.label = bt.label;
                k.key = bt.trigger;
                k.live = bt.action != 0 || router_.edge_for(trigger_for_control(screen, bt.trigger)) != nullptr;
                under.push_back(std::move(k));
            }
            draw_shell_root(b, under);
            draw_quit_dialog(b, ctls);
        } else {
            draw_shell_root(b, ctls);
        }
    } else if (screen == ScreenId::Home) {
        draw_home(b, ctls);
        for (const Ctl& c : ctls)
            if (c.key == "PlayButton")
                card(b, c.x0, c.y0, c.x1, c.y1, "PLAY", "Versus A.I., a custom map, or a replay.", "-> PLAY", c.look);
    } else if (screen == ScreenId::VersusAI) {
        draw_play_hub(b, ctls);
    } else if (screen == ScreenId::Profile) {
        ProfileInfo info;
        info.name = settings_.profile_name();
        info.editing = profile_editing_;
        info.matches = settings_.get_int(ShellSettings::kProfileMatchesKey, 0);
        info.wins = settings_.get_int(ShellSettings::kProfileWinsKey, 0);
        info.caret = fx_.pulse();
        draw_profile(b, ctls, info);
    } else if (screen == ScreenId::Custom) {
        for (Ctl& c : ctls) c.selected = c.action >= kMapRow0 && (c.action - kMapRow0) == sel_map_;
        CustomInfo info;
        info.selected = sel_map_ >= 0 && sel_map_ < int(maps_.size()) ? maps_[std::size_t(sel_map_)].name : std::string("-");
        info.size_line = sel_map_ > 0 && sel_map_ < int(maps_.size())
                             ? (maps_[std::size_t(sel_map_)].casc ? "Install map (CASC)" : maps_[std::size_t(sel_map_)].path)
                             : std::string("Built-in arena: 72x72, no cliffs");
        const int page = custom_rows_per_page(view_h);
        const int last = std::min(int(maps_.size()), custom_scroll_ + page);
        info.count_line = int(maps_.size()) > 1
                              ? std::to_string(maps_.size() - 1) + " maps, showing " + std::to_string(custom_scroll_ + 1) + "-" + std::to_string(last)
                              : std::string("no .SC2Map found - the built-in arena only");
        info.error = match_error_;
        info.preview = map_preview(sel_map_);
        info.preview_pending = false;
        draw_custom(b, ctls, info);
    } else if (screen == ScreenId::ReplayBrowse) {
        band_flow_strip(b, "REPLAYS");
        for (Ctl& c : ctls) c.selected = c.action >= kReplayRow0 && c.action - kReplayRow0 == sel_replay_;
        for (const Ctl& c : ctls) {
            if (c.action == kActWatch) button_primary(b, c.x0, c.y0, c.x1, c.y1, "WATCH", theme::kTypePrimaryLg, c.look);
            else if (c.action == kActBack) button_secondary(b, c.x0, c.y0, c.x1, c.y1, "BACK", theme::kTypeSecondaryLg, c.look);
            else if (c.action >= kReplayRow0) list_row(b, c.x0, c.y0, c.x1, c.y1, c.label, c.selected, c.look);
        }
        if (replays_.empty())
            text_at(b, b.s(theme::kScreenPad), b.s(theme::kFlowStripH) + b.s(theme::kPagePad) * 2,
                    replays_dir_.empty() ? "no replays folder is configured - every finished match would be recorded there"
                                         : "no replays in " + replays_dir_ + " - a finished match is recorded there",
                    theme::kTypeBody, theme::kTextMuted);
        else if (sel_replay_ < 0)
            text_at(b, b.s(theme::kScreenPad), b.by(theme::kFooterH + theme::kPagePad + theme::kPrimaryH + theme::kPagePad),
                    "select a recording, then WATCH", theme::kTypeCaption, theme::kTextMuted);
        if (!match_error_.empty())
            text_at(b, b.s(theme::kScreenPad), b.by(theme::kFooterH + theme::kPagePad), match_error_, theme::kTypeCaption, theme::kErr);
        band_footer(b);
    } else if (screen == ScreenId::Options) {
        OptionsInfo info;
        info.engine_page = options_engine_page_;
        info.selected_rail = options_engine_page_ ? kRailEngine : (options_panel_ < kRailHotkeys ? options_panel_ : options_panel_ + 1);
        info.documents = int(screen_layouts(ScreenId::Options).size());
        for (const EngineRow& r : engine_rows()) {
            const std::string key = r.key;
            std::string v;
            float f = 0.0f;
            bool bus_row = false;
            for (const ShellSettings::Bus& bus : ShellSettings::buses())
                if (std::string("opt/") + bus.key == key) {
                    f = settings_.bus_gain(bus);
                    v = std::to_string(int(f * 100.0f + 0.5f)) + " %";
                    bus_row = true;
                }
            if (!bus_row) {
                if (key == "opt/gameplay.unit_status") v = ShellSettings::unit_status_names()[std::size_t(settings_.unit_status_mode())];
                else if (key == "opt/gameplay.game_speed") v = ShellSettings::speed_name(settings_.game_speed());
                else if (key == "opt/hotkey.profile") v = ShellSettings::hotkey_profiles()[std::size_t(settings_.hotkey_profile())].name;
                else if (key == "opt/display.mode") v = ShellSettings::display_mode_name(settings_.display_mode());
                else if (key == "opt/display.vsync") { f = settings_.vsync() ? 1.0f : 0.0f; v = settings_.vsync() ? "On" : "Off"; }
                else if (key == "opt/display.ui_scale") {
                    const float s = settings_.ui_scale();
                    f = (s - kUiScaleMin) / (kUiScaleMax - kUiScaleMin);
                    v = format_ui_scale(s);
                }
            }
            info.row_values.push_back(v);
            info.row_fracs.push_back(f);
        }
        draw_options(b, ctls, info);
        if (!options_engine_page_)
            for (const Ctl& c : ctls) {
                if (!c.selected || c.key.size() <= 8 || c.key.compare(c.key.size() - 8, 8, "CheckBox") != 0) continue;
                const int inset = std::max(2, (c.x1 - c.x0) / 5);
                draw::fill_a(b.out, b.w, b.h, c.x0 + inset, c.y0 + inset, c.x1 - inset, c.y1 - inset,
                             theme::kAccent.r, theme::kAccent.g, theme::kAccent.b, 255);
            }
    } else if (screen == ScreenId::PauseMenu) {
        draw_pause(b, ctls);
    } else if (screen == ScreenId::Loading) {
        LoadInfo info;
        info.map_name = sel_map_ >= 0 && sel_map_ < int(maps_.size()) ? maps_[sel_map_].name
                                                                     : std::string("-");
        info.p0_race = race_name(race_of(0));
        info.p1_race = race_name(race_of(1));
        const std::vector<data::TeamColor> tc = data::team_colors(cat_);
        auto to_rgb = [](const float v[4]) {
            auto ch = [](float f) {
                const int n = int(f * 255.0f + 0.5f);
                return std::uint8_t(n < 0 ? 0 : (n > 255 ? 255 : n));
            };
            return theme::Rgb{ch(v[0]), ch(v[1]), ch(v[2])};
        };
        static const bool raw_slots = std::getenv("SC2_SHELL_TEAM_SLOT_RAW") != nullptr;
        const data::TeamColor* c0 = nullptr;
        const data::TeamColor* c1 = nullptr;
        for (const data::TeamColor& t : tc) {
            if (t.slot == (raw_slots ? 0 : 1)) c0 = &t;
            if (t.slot == (raw_slots ? 1 : 2)) c1 = &t;
        }
        if (c0 && c1) {
            info.p0_color = to_rgb(c0->diffuse);
            info.p1_color = to_rgb(c1->diffuse);
            info.have_colors = true;
        }
        info.done = int(load_plan_.done);
        info.total = int(load_plan_.steps.size());
        info.stage = load_plan_.label;
        info.pulse = fx_.pulse();
        if (!replay_pending_) {
            info.difficulty = ShellSettings::difficulty_name(settings_.ai_difficulty());
            info.speed = ShellSettings::speed_name(settings_.game_speed());
        }
        draw_loading(b, info);
    } else if (screen == ScreenId::Lobby) {
        for (Ctl& c : ctls) c.selected = c.action >= kMapRow0 && (c.action - kMapRow0) == sel_map_;
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
        info.host_name = settings_.profile_name();
        info.preview = map_preview(sel_map_);
        draw_lobby(b, ctls, info, kActStart, kActBack, kActMapsFolder, kMapRow0);
    } else {
        draw_glue_screen(screen, view_w, view_h, out);
        for (const Ctl& c : ctls) {
            if (c.x1 <= c.x0 || c.y1 <= c.y0) continue;
            button_menu(b, c.x0, c.y0, c.x1, c.y1, c.label, false, c.live, c.look);
        }
        if (screen == ScreenId::Score) draw_score_table(view_w, view_h, out);
    }

    draw_toasts(b, fx_.toasts());
    if (fx_.tip_due() && !fx_.tip_key.empty()) {
        std::string tip = shell_tooltip(fx_.tip_key);
        if (tip.empty() && screen == ScreenId::Lobby) {
            for (const Ctl& c : ctls)
                if (c.key == fx_.tip_key) {
                    if (c.action == kActP0Race) tip = shell_tooltip("Player race");
                    else if (c.action == kActP1Race) tip = shell_tooltip("AI race");
                    else if (c.action == kActAiDiff) tip = shell_tooltip("AI difficulty");
                    else if (c.action == kActSpeed) tip = shell_tooltip("Game speed");
                    else if (c.action >= kMapRow0) tip = maps_[std::size_t(c.action - kMapRow0)].casc ? "An install map, read from the CASC store" : maps_[std::size_t(c.action - kMapRow0)].path;
                }
        }
        if (tip.empty() && screen == ScreenId::Custom)
            for (const Ctl& c : ctls)
                if (c.key == fx_.tip_key && c.action >= kMapRow0)
                    tip = maps_[std::size_t(c.action - kMapRow0)].casc ? "An install map, read from the CASC store" : maps_[std::size_t(c.action - kMapRow0)].path;
        draw_tooltip(b, int(mouse_x_), int(mouse_y_), tip);
    }
}

void FrontEnd::request_boot_screen(const std::string& name) { boot_screen_request() = name; }

bool FrontEnd::enter_screen(const std::string& boot_name) {
    const ScreenId id = screen_from_boot_name(boot_name);
    switch (id) {
        case ScreenId::Count:
        case ScreenId::Boot:
        case ScreenId::Login:
        case ScreenId::Loading:
        case ScreenId::InGame:
        case ScreenId::PauseMenu:
        case ScreenId::ReplayControls:
            return false;
        case ScreenId::Sandbox:
            start_sandbox();
            return true;
        case ScreenId::Options:
        case ScreenId::Profile:
            router_.unwind_to(ScreenId::GlueRoot);
            router_.push(id);
            return true;
        case ScreenId::Hotkeys:
            router_.unwind_to(ScreenId::GlueRoot);
            router_.push(ScreenId::Options);
            router_.push(id);
            return true;
        case ScreenId::Portraits:
            router_.unwind_to(ScreenId::GlueRoot);
            router_.push(ScreenId::Profile);
            router_.push(id);
            return true;
        default:
            router_.unwind_to(id);
            return true;
    }
}

bool FrontEnd::run_shell_screen(shell::Pass pass, std::uint32_t view_w, std::uint32_t view_h, const render::InputState* in,
                                std::vector<std::uint8_t>* out, float dt_ms, std::vector<shell::Control>* controls, bool* escape) {
    if (shell_v1() || !view_w || !view_h) return false;
    const shell::ScreenFn fn = shell::screen_fn(router_.top());
    if (!fn) return false;
    shell::Faces faces;
    faces.face[0] = &overlay_font_;
    faces.face[1] = &overlay_font_ext_;
    faces.face[2] = &overlay_font_bold_;
    shell::ShellCtx ctx{*this, settings_, fx_, router_};
    shell::Ui ui(pass, view_w, view_h, in, out, fx_, ui_, faces, dt_ms, double_click_ms_ > 0.0 ? double_click_ms_ : 500.0);
    ui.paste = clipboard_cb_;
    if (!fn(ui, ctx)) return false;
    ui.finish();
    if (controls) *controls = ui.controls();
    if (escape) *escape = ui.escape_pending();
    return true;
}

bool FrontEnd::shell_v2_screen() { return run_shell_screen(shell::Pass::Collect, 1280, 720, nullptr, nullptr, 0.0f, nullptr, nullptr); }

std::vector<shell::Control> FrontEnd::shell_controls(std::uint32_t view_w, std::uint32_t view_h) {
    std::vector<shell::Control> cs;
    run_shell_screen(shell::Pass::Collect, view_w, view_h, nullptr, nullptr, 0.0f, &cs, nullptr);
    return cs;
}

void FrontEnd::remember_last_match() {
    if (shell_v1() || !skirmish_ || skirmish_->replay_active()) return;
    const MatchResult::Outcome o = skirmish_->outcome();
    if (o == MatchResult::Outcome::Ongoing) return;
    ShellSettings::LastMatch m;
    m.outcome = o == MatchResult::Outcome::Victory ? "victory" : "defeat";
    for (const platform::MapEntry& e : maps_)
        if (e.path == match_map_path_) {
            m.map = e.name;
            break;
        }
    m.race[0] = int(race_of(0));
    m.race[1] = int(race_of(1));
    m.ticks = static_cast<long long>(skirmish_->world().tick());
    m.replay = last_recorded_replay_;
    m.tier = settings_.ai_difficulty();
    const auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    char stamp[32];
    std::tm tmv{};
#ifdef _WIN32
    localtime_s(&tmv, &now);
#else
    localtime_r(&now, &tmv);
#endif
    std::strftime(stamp, sizeof stamp, "%Y%m%d-%H%M%S", &tmv);
    m.time = stamp;
    settings_.set_last_match(m);
}

namespace shell {

const std::vector<platform::MapEntry>& ShellCtx::maps() const { return fe.maps_; }
int ShellCtx::selected_map() const { return fe.sel_map_; }

void ShellCtx::select_map(int index) {
    if (index < 0 || index >= int(fe.maps_.size())) return;
    const bool changed = fe.sel_map_ != index;
    fe.sel_map_ = index;
    fe.match_error_.clear();
    settings.set_lobby_map(fe.maps_[std::size_t(index)].path);
    if (changed && fe.lobby_format_ != FrontEnd::LobbyFormat::OneVsOne && !fe.set_lobby_format(fe.lobby_format_)) {
        fe.lobby_format_ = FrontEnd::LobbyFormat::OneVsOne;
        fe.lobby_slots_.clear();
        fx.toast(std::string("This map has ") + std::to_string(fe.map_start_count(index)) + " start locations - back to 1v1", false);
    }
}

const IconAtlas::Icon* ShellCtx::map_preview(int index) { return fe.map_preview(index); }

const IconAtlas::Icon* ShellCtx::map_thumb(int index) {
    if (index <= 0 || index >= int(fe.maps_.size())) return nullptr;
    const auto it = fe.thumb_cache_.find(fe.maps_[std::size_t(index)].path);
    return it != fe.thumb_cache_.end() && it->second.ok() ? &it->second : nullptr;
}

void ShellCtx::warm_map_thumbs(int budget) {
    for (; budget > 0 && fe.thumb_cursor_ < fe.maps_.size(); ++fe.thumb_cursor_) {
        const platform::MapEntry& e = fe.maps_[fe.thumb_cursor_];
        if (e.path.empty() || fe.thumb_cache_.count(e.path)) continue;
        --budget;
        IconAtlas::Icon small;
        try {
            std::vector<std::uint8_t> bytes;
            if (e.casc) bytes = fe.store_.read_by_name(e.path);
            else {
                std::ifstream in(e.path, std::ios::binary);
                bytes.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
            }
            formats::Mpq arc = formats::Mpq::open(std::move(bytes));
            if (arc.contains("Minimap.tga")) {
                formats::TgaInfo ti;
                const std::vector<std::uint8_t> px = formats::decode_tga(arc.read_file("Minimap.tga"), ti);
                if (ti.width && ti.height && px.size() == std::size_t(ti.width) * ti.height * 4) {
                    const std::uint32_t side = 64;
                    small.w = small.h = side;
                    small.rgba.assign(std::size_t(side) * side * 4, 0);
                    draw::blit_fit(small.rgba, side, side, 0, 0, int(side), int(side), px.data(), std::uint32_t(ti.width), std::uint32_t(ti.height));
                }
            }
        } catch (const std::exception&) {
        }
        fe.thumb_cache_[e.path] = std::move(small);
    }
}

void ShellCtx::pick_maps_folder() {
    if (fe.maps_folder_cb_) fe.maps_folder_cb_();
}

bool ShellCtx::team_color(int slot, theme::Rgb& out) {
    if (!fe.team_colors_read_) {
        fe.team_colors_read_ = true;
        fe.team_colors_ = data::team_colors(fe.cat_);
    }
    static const bool raw = std::getenv("SC2_SHELL_TEAM_SLOT_RAW") != nullptr;
    const int want = raw ? slot : slot + 1;
    auto ch = [](float f) {
        const int n = int(f * 255.0f + 0.5f);
        return std::uint8_t(n < 0 ? 0 : (n > 255 ? 255 : n));
    };
    for (const data::TeamColor& tc : fe.team_colors_) {
        if (tc.slot != want) continue;
        out = theme::Rgb{ch(tc.diffuse[0]), ch(tc.diffuse[1]), ch(tc.diffuse[2])};
        return true;
    }
    return false;
}

int ShellCtx::first_real_map() const {
    for (std::size_t i = 0; i < fe.maps_.size(); ++i)
        if (!fe.maps_[i].path.empty()) return int(i);
    return -1;
}

int ShellCtx::setup_map() const {
    const std::string stored = settings.lobby_map();
    if (!stored.empty())
        for (std::size_t i = 0; i < fe.maps_.size(); ++i)
            if (fe.maps_[i].path == stored) return int(i);
    return first_real_map();
}

std::string ShellCtx::map_title(int index) const {
    return index >= 0 && index < int(fe.maps_.size()) ? fe.maps_[std::size_t(index)].name : std::string();
}

std::string ShellCtx::stored_map_label() const {
    std::string p = settings.lobby_map();
    const std::size_t slash = p.find_last_of("/\\");
    if (slash != std::string::npos) p = p.substr(slash + 1);
    const std::size_t dot = p.find_last_of('.');
    if (dot != std::string::npos && dot > 0) p = p.substr(0, dot);
    return p.empty() ? std::string("The stored map") : p;
}

int ShellCtx::map_by_title(const std::string& title) const {
    for (std::size_t i = 0; i < fe.maps_.size(); ++i)
        if (fe.maps_[i].name == title) return int(i);
    return -1;
}

bool ShellCtx::start_match(const char* via) {
    const std::string stored = settings.lobby_map();
    int index = -1;
    if (!stored.empty()) {
        for (std::size_t i = 0; i < fe.maps_.size(); ++i)
            if (fe.maps_[i].path == stored) index = int(i);
        if (index < 0) return false;
    } else {
        index = first_real_map();
    }
    if (index < 0) return false;
    fe.sel_map_ = index;
    return start_selected_match(via);
}

bool ShellCtx::start_selected_match(const char* via) {
    if (fe.sel_map_ < 0 || fe.sel_map_ >= int(fe.maps_.size()) || !router.edge_for(via)) return false;
    fe.replay_pending_ = false;
    fe.p0_pick_ = settings.profile_race();
    fe.p1_pick_ = settings.opponent_race();
    fe.match_seed_ = new_match_seed();
    const std::uint64_t match_seed = fe.match_seed_ ? fe.match_seed_ : MeleeGameConfig{}.seed;
    fe.match_error_.clear();
    if (fe.lobby_format_ == FrontEnd::LobbyFormat::OneVsOne || fe.lobby_slots_.empty()) {
        fe.set_two_player_races(resolve_lobby_race(fe.p0_pick_, match_seed, 0), resolve_lobby_race(fe.p1_pick_, match_seed, 1));
    } else {
        std::vector<MeleeSlot> s;
        for (std::size_t i = 0; i < fe.lobby_slots_.size(); ++i) {
            const FrontEnd::LobbySlot& row = fe.lobby_slots_[i];
            if (row.controller == MeleeSlot::Controller::Closed) continue;
            MeleeSlot sl;
            sl.race = resolve_lobby_race(row.pick, match_seed, int(i));
            sl.controller = row.controller;
            sl.difficulty = row.controller == MeleeSlot::Controller::AI ? row.difficulty : 0;
            sl.team = row.team;
            s.push_back(sl);
        }
        bool shared = false, one_side = !s.empty();
        for (std::size_t a = 0; a < s.size(); ++a)
            for (std::size_t b = a + 1; b < s.size(); ++b) {
                if (s[a].team >= 0 && s[a].team == s[b].team) shared = true;
                if (s[a].team < 0 || s[a].team != s[b].team) one_side = false;
            }
        if (!shared)
            for (MeleeSlot& sl : s) sl.team = -1;
        if (s.size() < 2) {
            fe.match_error_ = "a melee needs two players or more - open a row";
            return false;
        }
        if (shared && one_side) {
            fe.match_error_ = "every player is on one team - nobody to fight";
            return false;
        }
        fe.slots_ = std::move(s);
    }
    fe.loading_frames_ = 0;
    fe.load_plan_.reset();
    return router.follow(via);
}

const std::string& ShellCtx::match_error() const { return fe.match_error_; }

bool ShellCtx::watch_replay(const std::string& path, const char* via) {
    if (!router.edge_for(via)) return false;
    OrderLog log;
    if (!read_order_log_file(path, log)) return false;
    fe.replay_log_pending_ = std::move(log);
    fe.replay_pending_ = true;
    fe.replay_name_ = std::filesystem::path(path).stem().string();
    fe.sel_map_ = 0;
    for (std::size_t i = 0; i < fe.maps_.size(); ++i)
        if (!fe.replay_log_pending_.map_id.empty() && fe.maps_[i].path == fe.replay_log_pending_.map_id) fe.sel_map_ = int(i);
    fe.slots_from_log(fe.replay_log_pending_);
    fe.match_error_.clear();
    fe.loading_frames_ = 0;
    fe.load_plan_.reset();
    return router.follow(via);
}

int ShellCtx::ticks_per_second() const { return sim::World::kTicksPerSecond; }

ShellCtx::LoadView ShellCtx::loading() const {
    LoadView v;
    v.map_index = fe.sel_map_;
    if (fe.sel_map_ >= 0 && fe.sel_map_ < int(fe.maps_.size()))
        v.map = fe.maps_[std::size_t(fe.sel_map_)].path.empty() ? std::string("TEST ARENA") : fe.maps_[std::size_t(fe.sel_map_)].name;
    v.race[0] = fe.race_of(0);
    v.race[1] = fe.race_of(1);
    v.players = fe.slots_;
    v.replay = fe.replay_pending_;
    v.done = fe.load_plan_.done;
    v.total = fe.load_plan_.steps.size();
    v.stage = fe.load_plan_.label;
    v.elapsed_ms = fe.loading_ms_;
    return v;
}

ShellCtx::ResultView ShellCtx::result() const {
    ResultView v;
    v.decided = fe.last_outcome_ != MatchResult::Outcome::Ongoing;
    v.victory = fe.last_outcome_ == MatchResult::Outcome::Victory;
    const ShellSettings::LastMatch last = settings.last_match();
    v.map = last.map;
    v.replay = last.replay;
    v.ticks = last.ticks;
    v.race[0] = Race(last.race[0]);
    v.race[1] = Race(last.race[1]);
    v.tier = last.tier;
    v.score = &fe.last_score_;
    v.players = &fe.last_players_;
    return v;
}

int ShellCtx::lobby_format() const { return int(fe.lobby_format_); }
bool ShellCtx::set_lobby_format(int format) { return fe.set_lobby_format(FrontEnd::LobbyFormat(std::max(0, std::min(4, format)))); }
int ShellCtx::lobby_rows() const { return int(fe.lobby_slots_.size()); }
int ShellCtx::map_start_count(int index) { return fe.map_start_count(index); }
void ShellCtx::lobby_row(int i, int& controller, int& race, int& difficulty, int& team) const {
    controller = 2, race = 0, difficulty = 0, team = -1;
    if (i < 0 || i >= int(fe.lobby_slots_.size())) return;
    const FrontEnd::LobbySlot& s = fe.lobby_slots_[std::size_t(i)];
    controller = int(s.controller);
    race = int(s.pick);
    difficulty = s.difficulty;
    team = s.team;
}
void ShellCtx::set_lobby_row(int i, int controller, int race, int difficulty, int team) {
    if (i < 0 || i >= int(fe.lobby_slots_.size())) return;
    FrontEnd::LobbySlot& s = fe.lobby_slots_[std::size_t(i)];
    const MeleeSlot::Controller c = MeleeSlot::Controller(std::max(0, std::min(2, controller)));
    if (c == MeleeSlot::Controller::Human && s.controller != MeleeSlot::Controller::Human)
        for (FrontEnd::LobbySlot& o : fe.lobby_slots_)
            if (o.controller == MeleeSlot::Controller::Human) {
                o.controller = MeleeSlot::Controller::AI;
                if (o.difficulty <= 0) o.difficulty = settings.ai_difficulty();
            }
    s.controller = c;
    s.pick = LobbyRace(std::max(0, std::min(3, race)));
    s.difficulty = std::max(0, std::min(7, difficulty));
    if (c == MeleeSlot::Controller::AI && s.difficulty == 0) s.difficulty = settings.ai_difficulty();
    s.team = std::max(-1, std::min(3, team));
    fe.match_error_.clear();
}
std::string ShellCtx::player_name() const { return settings.profile_name(); }

Race ShellCtx::player_default_race() const {
    const LobbyRace r = settings.profile_race();
    return r == LobbyRace::Random ? Race::Terran : Race(int(r));
}

const IconAtlas::Icon* ShellCtx::player_portrait() { return portrait(PortraitRef::parse(settings.profile_portrait())); }
const IconAtlas::Icon* ShellCtx::portrait(const PortraitRef& ref) { return fe.portraits_.image(fe.cat_, fe.store_, ref, player_default_race()); }
const IconAtlas::Icon* ShellCtx::portrait_texture(const std::string& name) { return fe.portraits_.texture(fe.store_, name); }
const IconAtlas::Icon* ShellCtx::ai_avatar() { return fe.portraits_.ai_avatar(fe.store_); }
const IconAtlas::Icon* ShellCtx::race_mark(LobbyRace race) { return fe.portraits_.race_mark(fe.store_, race); }
PortraitCatalog& ShellCtx::portraits() { return fe.portraits_; }
bool ShellCtx::scan_portraits(int budget) { return fe.portraits_.scan(fe.cat_, fe.store_, budget); }

void ShellCtx::quit() { router.follow("Esc / close client"); }
const char* ShellCtx::version() const { return sc2::kVersion; }
bool ShellCtx::has_maps_folder_picker() const { return bool(fe.maps_folder_cb_); }

ShellCtx::GameData ShellCtx::game_data() {
    if (!fe.game_data_probed_) {
        fe.game_data_probed_ = true;
        const std::string root = settings.get("install_root");
        if (!root.empty()) fe.game_data_ = probe_install(root);
    }
    GameData g;
    g.version = fe.game_data_.version;
    g.valid = fe.game_data_.valid;
    g.verified = fe.game_data_.valid && fe.game_data_.compatible;
    return g;
}

}

}
