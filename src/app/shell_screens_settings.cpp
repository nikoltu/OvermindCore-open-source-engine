#include "omc/app/shell_screens.hpp"

#include <algorithm>
#include <cmath>

namespace omc::app::shell {

namespace v2 = theme::v2;
namespace L = layout::s60_settings;

namespace {

enum Tab { kGameplay, kVideo, kAudio, kPerformance, kGameData, kTabCount };
const char* const kTabNames[kTabCount] = {"Gameplay", "Video", "Audio", "Performance", "Game data"};

struct RowInfo {
    std::string id;
    std::string name;
    std::string help;
    std::string dflt;
    std::string applies;
};

constexpr float kRowX = 88.0f;
constexpr float kRowW = 1301.0f;
constexpr float kRowY = 232.0f;
constexpr float kRowPitch = 72.0f;
constexpr float kRowH = 64.0f;
constexpr float kControlW = 560.0f;
constexpr float kPad = 24.0f;

std::vector<std::string> names_of(const std::vector<ShellSettings::Level>& levels) {
    std::vector<std::string> out;
    for (const ShellSettings::Level& l : levels) out.push_back(l.name);
    return out;
}

std::vector<std::string> wrap(const Ui& ui, const std::string& s, theme::Type role, int width) {
    std::vector<std::string> lines;
    std::string line, word;
    auto flush_word = [&]() {
        if (word.empty()) return;
        const std::string cand = line.empty() ? word : line + " " + word;
        if (!line.empty() && ui.text_width(cand, role) > width) {
            lines.push_back(line);
            line = word;
        } else {
            line = cand;
        }
        word.clear();
    };
    for (char c : s) {
        if (c == ' ') flush_word();
        else if (c == '\n') {
            flush_word();
            lines.push_back(line);
            line.clear();
        } else {
            word.push_back(c);
        }
    }
    flush_word();
    if (!line.empty()) lines.push_back(line);
    return lines;
}

struct Page {
    Ui& ui;
    ShellCtx& ctx;
    std::vector<RowInfo> rows;
    int line = 1;

    Rect row_rect(int i) const { return ui.at(kRowX, kRowY + kRowPitch * float(i), kRowW, kRowH); }
    Rect control_rect(int i) const { return ui.at(kRowX + kRowW - kControlW - kPad, kRowY + kRowPitch * float(i), kControlW, kRowH); }

    Rect row(const RowInfo& info) {
        const int i = int(rows.size());
        rows.push_back(info);
        const Rect r = row_rect(i);
        const Rect c = control_rect(i);
        ui.text(Rect{r.x0 + ui.px(kPad), r.y0, c.x0 - ui.px(kPad), r.y1}, info.name, v2::kBody, v2::kTextHi);
        ui.fill(Rect{r.x0, r.y1 + ui.px(4.0f), r.x1, r.y1 + ui.px(4.0f) + line}, v2::kLine1);
        ui.publish_region("settings/row/" + info.id, r, info.name);
        return c;
    }

    void changed(const std::string& id) {
        ui.state().str["settings/help"] = id;
        ctx.apply_settings();
    }
};

void saved(ShellCtx& ctx, bool ok) {
    if (!ok) ctx.fx.toast("Setting could not be saved", true);
}

void gameplay(Page& p) {
    ShellCtx& ctx = p.ctx;
    Ui& ui = p.ui;
    {
        const Rect c = p.row({"speed", "Game speed", "How fast a match runs. Ladder melee runs at Faster; the + and - keys change it in a match.",
                              "Faster", "Next match, at once in a match"});
        int v = int(ctx.settings.game_speed());
        Opts o;
        o.hint = "How fast a match runs.";
        if (ui.segmented("set/speed", c, {"Slower", "Slow", "Normal", "Fast", "Faster"}, v, o)) {
            saved(ctx, ctx.settings.set(ShellSettings::kGameSpeedKey, std::to_string(v)));
            p.changed("speed");
        }
    }
    {
        const Rect c = p.row({"ai", "A.I. difficulty", "The computer's tier: worker delay, actions a minute, when it retreats.", "Medium", "Next match"});
        int v = ctx.settings.ai_difficulty();
        Opts o;
        o.hint = "The computer's tier.";
        if (ui.stepper("set/ai", c, v, 1, 7, ShellSettings::difficulty_name(v), o)) {
            saved(ctx, ctx.settings.set(ShellSettings::kAiDifficultyKey, std::to_string(v)));
            p.changed("ai");
        }
    }
    {
        const Rect c = p.row({"life_bars", "Unit life bars",
                              "When the bars over units show: Normal (Alt reveals them), Targeting, Selected, Damaged or Always.", "Normal", "At once"});
        int v = int(ctx.settings.unit_status_mode());
        std::vector<std::string> items;
        for (const char* n : ShellSettings::unit_status_names()) items.push_back(n);
        Opts o;
        o.hint = "When the bars over units show.";
        if (ui.dropdown("set/life_bars", c, items, v, o)) {
            saved(ctx, ctx.settings.set(ShellSettings::kUnitStatusKey, std::to_string(v)));
            p.changed("life_bars");
        }
    }
    {
        const Rect c = p.row({"badges", "Key badges on buttons", "Shows the hotkey letter on the menu buttons.", "On", "At once"});
        bool v = ctx.settings.hotkey_badges();
        Opts o;
        o.hint = "Shows the hotkey letter on the menu buttons.";
        if (ui.toggle("set/badges", Rect{c.x0, c.y0, c.x0 + ui.px(240.0f), c.y1}, v, o)) {
            saved(ctx, ctx.settings.set(ShellSettings::kHotkeyBadgesKey, v ? "1" : "0"));
            p.changed("badges");
        }
    }
}

void video(Page& p) {
    ShellCtx& ctx = p.ctx;
    Ui& ui = p.ui;
    const Rect c = p.row({"bg_motion", "Background motion", "The slow moving lines behind the menus. Off draws a still background and costs less.",
                          "On", "At once"});
    bool v = ctx.settings.bg_motion();
    Opts o;
    o.hint = "The moving lines behind the menus.";
    if (ui.toggle("set/bg_motion", Rect{c.x0, c.y0, c.x0 + ui.px(240.0f), c.y1}, v, o)) {
        saved(ctx, ctx.settings.set(ShellSettings::kBgMotionKey, v ? "1" : "0"));
        p.changed("bg_motion");
    }
}

void audio(Page& p) {
    ShellCtx& ctx = p.ctx;
    Ui& ui = p.ui;
    for (const ShellSettings::Bus& bus : ShellSettings::buses()) {
        const std::string id = bus.key;
        const Rect c = p.row({id, bus.label, std::string(bus.label) + " volume. Master scales every bus.", "100 %", "Next match, at once in a match"});
        float v = ctx.settings.bus_gain(bus);
        const std::string text = std::to_string(int(std::lround(v * 100.0f))) + " %";
        Opts o;
        o.hint = std::string(bus.label) + " volume.";
        if (ui.slider("set/" + id, c, v, text, o)) {
            saved(ctx, ctx.settings.set_bus_gain(bus, v));
            p.changed(id);
        }
    }
}

void performance(Page& p) {
    ShellCtx& ctx = p.ctx;
    Ui& ui = p.ui;
    struct Perf {
        const char* id;
        const char* name;
        const char* help;
        const char* key;
        const std::vector<ShellSettings::Level>& levels;
        int dflt;
        int value;
    };
    {
        const ShellSettings::Preset& dp = ShellSettings::kPresets[ShellSettings::kDefaultPreset];
        const Rect c = p.row({"preset", "Graphics quality",
                              "Sets the five scene rows below at once. Ultra is the full look; High, Medium and Low trade detail for GPU time. "
                              "Change any row afterwards; no level is lit when the rows match no preset.",
                              dp.name, "At once"});
        int v = ctx.settings.quality_preset();
        Opts o;
        o.hint = "Graphics quality";
        if (ui.segmented("set/preset", c, ShellSettings::preset_names(), v, o)) {
            saved(ctx, ctx.settings.set_quality_preset(v));
            p.changed("preset");
        }
    }
    const Perf perfs[] = {
        {"shadows", "Shadow quality",
         "Which models cast shadows. Units only skips the map's doodads, foliage and cliffs; Off skips the shadow pass. Saves GPU time on a phone.",
         ShellSettings::kShadowQualityKey, ShellSettings::shadow_quality_levels(), ShellSettings::kShadowQualityDefault,
         ctx.settings.shadow_quality_level()},
        {"shadow_map", "Shadow detail", "The size of the shadow map. 1024 and 512 draw softer, blockier shadow edges and cost less to draw and sample.",
         ShellSettings::kShadowMapKey, ShellSettings::shadow_map_levels(), ShellSettings::kShadowMapDefault, ctx.settings.shadow_map_level()},
        {"terrain", "Terrain quality",
         "How the ground textures blend. Medium blends at most three layers with less sharpening and filtering; Low blends two layers without "
         "surface relief. The largest GPU cost on integrated graphics.",
         ShellSettings::kTerrainQualityKey, ShellSettings::terrain_quality_levels(), ShellSettings::kTerrainQualityDefault,
         ctx.settings.terrain_quality_level()},
        {"doodads", "Doodad density",
         "Leaves out small decorative doodads: Medium the smallest, Low the small and mid-sized ones. Doodads that block paths or placement and "
         "doodads that cast light always stay.",
         ShellSettings::kDoodadDensityKey, ShellSettings::doodad_density_levels(), ShellSettings::kDoodadDensityDefault,
         ctx.settings.doodad_density_level()},
        {"render_scale", "Render resolution", "The 3D scene is drawn at this share of the screen size and scaled up; the HUD stays sharp. 75 % or 50 % saves GPU time.",
         ShellSettings::kRenderScaleKey, ShellSettings::render_scale_levels(), ShellSettings::kRenderScaleDefault,
         ctx.settings.render_scale_level()},
        {"portrait_tex", "Portrait texture quality",
         "The largest side of a texture on the talking portrait. 1024 or 512 uses less memory and uploads faster on a phone.",
         ShellSettings::kPortraitTextureKey, ShellSettings::portrait_texture_levels(), ShellSettings::kPortraitTextureDefault,
         ctx.settings.portrait_texture_level()},
        {"console_tex", "Console texture quality", "The console frame around the minimap, portrait and command card. Half draws it from half-size textures.",
         ShellSettings::kConsoleTextureKey, ShellSettings::console_texture_levels(), ShellSettings::kConsoleTextureDefault,
         ctx.settings.console_texture_level()},
        {"portrait_rate", "Portrait frame rate", "How often the talking portrait is drawn again. A lower rate frees the frame for the match.",
         ShellSettings::kPortraitRateKey, ShellSettings::portrait_rate_levels(), ShellSettings::kPortraitRateDefault,
         ctx.settings.portrait_rate_level()},
    };
    for (const Perf& f : perfs) {
        const Rect c = p.row({f.id, f.name, f.help, f.levels[std::size_t(f.dflt)].name, "At once"});
        int v = f.value;
        Opts o;
        o.hint = f.name;
        if (ui.segmented(std::string("set/") + f.id, c, names_of(f.levels), v, o)) {
            saved(ctx, ctx.settings.set_level(f.key, f.levels, v));
            p.changed(f.id);
        }
    }
    const Rect r = p.row_rect(int(p.rows.size()));
    ui.text(Rect{r.x0 + ui.px(kPad), r.y0, r.x1 - ui.px(kPad), r.y1}, "Lower these when a match stutters. Each change applies at once.", v2::kSmall,
            v2::kTextMuted);
}

void game_data(Page& p) {
    ShellCtx& ctx = p.ctx;
    Ui& ui = p.ui;
    const ShellCtx::GameData g = ctx.game_data();
    const std::string root = ctx.settings.get("install_root");
    const std::string maps = ctx.settings.get("maps_dir");
    const std::string replays = ctx.settings.get("replays_dir");
    const std::string build = g.version.empty() ? std::string("Unknown") : g.version + (g.verified ? "  -  verified" : "  -  unverified build");
    const RowInfo infos[] = {
        {"folder", "Game folder", "Where the client reads the game data from.", "Asked on the first start", "After restart"},
        {"build", "Game build", "The build of the game data in that folder.", "", ""},
        {"maps", "Maps folder", "A folder of your own maps; the install's maps are always listed.", "The game folder", "At once"},
        {"replays", "Replays folder", "Where the recordings of your matches are written.", "Beside the settings file", "At once"},
    };
    const std::string values[] = {root.empty() ? std::string("Not set") : root, build, maps.empty() ? std::string("The game folder") : maps,
                                  replays.empty() ? std::string("Beside the settings file") : replays};
    for (int i = 0; i < 4; ++i) {
        p.row(infos[i]);
        const Rect r = p.row_rect(i);
        const Rect v{r.x0 + ui.px(400.0f), r.y0, r.x1 - ui.px(kPad), r.y1};
        ui.text(v, values[i], v2::kBody, i == 1 && !g.verified ? v2::kWarn : v2::kTextBody, Align::Right);
    }
    if (ctx.router.edge_for("GameDataButton")) {
        const Rect r = p.row_rect(4);
        Opts o;
        o.trigger = "GameDataButton";
        o.hint = "Point the client at another game folder.";
        if (ui.button("GameDataButton", Rect{r.x0 + ui.px(kPad), r.y0, r.x0 + ui.px(kPad + 400.0f), r.y1}, "Change game folder", ButtonKind::Normal, o))
            ctx.router.follow("GameDataButton");
    }
}

std::vector<std::string> tab_keys(int tab) {
    switch (tab) {
        case kGameplay:
            return {ShellSettings::kGameSpeedKey, ShellSettings::kAiDifficultyKey, ShellSettings::kUnitStatusKey, ShellSettings::kHotkeyBadgesKey};
        case kVideo: return {ShellSettings::kBgMotionKey};
        case kAudio: {
            std::vector<std::string> k;
            for (const ShellSettings::Bus& b : ShellSettings::buses()) k.push_back(b.key);
            return k;
        }
        case kPerformance:
            return {ShellSettings::kPortraitTextureKey, ShellSettings::kConsoleTextureKey, ShellSettings::kPortraitRateKey, ShellSettings::kShadowQualityKey,
                    ShellSettings::kRenderScaleKey,     ShellSettings::kShadowMapKey,      ShellSettings::kTerrainQualityKey, ShellSettings::kDoodadDensityKey};
        default: return {};
    }
}

void help_pane(Ui& ui, const Page& p) {
    const Rect pane = ui.at(L::kHelpPane);
    ui.panel(pane, "Help");
    if (p.rows.empty()) return;
    const UiState& st = ui.state();
    const RowInfo* pick = nullptr;
    for (const std::string* probe : {&st.hot, &st.focus}) {
        for (const RowInfo& r : p.rows) {
            const std::string pre = "set/" + r.id;
            if (probe->compare(0, pre.size(), pre) == 0 && (probe->size() == pre.size() || (*probe)[pre.size()] == '/')) pick = &r;
            if (*probe == "settings/row/" + r.id) pick = &r;
        }
        if (pick) break;
    }
    if (!pick) {
        const auto it = st.str.find("settings/help");
        if (it != st.str.end())
            for (const RowInfo& r : p.rows)
                if (r.id == it->second) pick = &r;
    }
    if (!pick) pick = &p.rows.front();
    const int pad = ui.px(kPad);
    int y = pane.y0 + ui.px(64.0f);
    ui.text(Rect{pane.x0 + pad, y, pane.x1 - pad, y + ui.px(40.0f)}, pick->name, v2::kH3, v2::kTextHi);
    y += ui.px(56.0f);
    const int lh = ui.px(32.0f);
    for (const std::string& l : wrap(ui, pick->help, v2::kBody, pane.w() - pad * 2)) {
        ui.text(Rect{pane.x0 + pad, y, pane.x1 - pad, y + lh}, l, v2::kBody, v2::kTextBody);
        y += lh;
    }
    y += ui.px(24.0f);
    const int kw = ui.px(160.0f);
    if (!pick->dflt.empty()) {
        ui.text(Rect{pane.x0 + pad, y, pane.x0 + pad + kw, y + lh}, "DEFAULT", v2::kLabel, v2::kTextMuted);
        ui.text(Rect{pane.x0 + pad + kw, y, pane.x1 - pad, y + lh}, pick->dflt, v2::kBody, v2::kTextBody);
        y += lh + ui.px(8.0f);
    }
    if (!pick->applies.empty()) {
        ui.text(Rect{pane.x0 + pad, y, pane.x0 + pad + kw, y + lh}, "APPLIES", v2::kLabel, v2::kTextMuted);
        ui.text(Rect{pane.x0 + pad + kw, y, pane.x1 - pad, y + lh}, pick->applies, v2::kBody, v2::kTextBody);
    }
    ui.publish_region("settings/help", pane, pick->name);
}

}

bool screen_settings(Ui& ui, ShellCtx& ctx) {
    UiState& st = ui.state();
    if (st.num["settings/authored"]) return false;
    const bool in_match = ctx.in_match();
    chrome(ui, ctx, "SETTINGS", kChromeBack | (in_match ? 0u : unsigned(kChromePlate)), "H Hotkeys   A Authored panels   ESC Back");
    ui.fluid({FluidSpan{L::kTabPage.x, L::kTabPage.x + L::kTabPage.w, 1.0f}});

    std::vector<std::string> names;
    const int tab_count = in_match ? kGameData : kTabCount;
    for (int i = 0; i < tab_count; ++i) names.push_back(kTabNames[i]);
    int& tab = st.num["settings/tab"];
    tab = std::max(0, std::min(tab_count - 1, tab));
    if (ui.tabs("settings/tabs", ui.at(L::kGameplay.x, L::kGameplay.y, L::kTabPage.w, L::kGameplay.h), names, tab)) st.str["settings/help"].clear();

    if (ctx.router.edge_for("HotkeysButton")) {
        Opts o;
        o.trigger = "HotkeysButton";
        o.hotkey = 'H';
        o.hint = "The hotkey editor.";
        if (ui.button("HotkeysButton", ui.at(L::kAuthoredPanels.x - 336.0f, L::kAuthoredPanels.y, L::kAuthoredPanels.w, L::kAuthoredPanels.h), "Hotkeys...",
                      ButtonKind::Ghost, o))
            ctx.router.follow("HotkeysButton");
    }
    {
        Opts o;
        o.hotkey = 'A';
        o.hint = "The install's own option panels, as the game draws them.";
        if (ui.button("settings/authored", ui.at(L::kAuthoredPanels), "Authored panels...", ButtonKind::Ghost, o)) {
            st.num["settings/authored"] = 1;
            return false;
        }
    }

    ui.panel(ui.at(L::kTabPage));
    Page page{ui, ctx, {}, std::max(1, ui.px(v2::kLineW))};
    switch (tab) {
        case kGameplay: gameplay(page); break;
        case kVideo: video(page); break;
        case kAudio: audio(page); break;
        case kPerformance: performance(page); break;
        default: game_data(page); break;
    }
    const std::vector<std::string> keys = tab_keys(tab);
    if (!keys.empty()) {
        Opts o;
        o.hint = "Puts every row on this tab back to its default.";
        if (ui.button("settings/restore", ui.at(L::kRestoreDefaults), "Restore defaults", ButtonKind::Normal, o)) {
            bool ok = true;
            for (const std::string& k : keys) ok = ctx.settings.set(k, "") && ok;
            saved(ctx, ok);
            ctx.apply_settings();
        }
    }
    help_pane(ui, page);
    toasts(ui, ctx);
    return true;
}

}
