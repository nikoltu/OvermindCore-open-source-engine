#include "sc2/app/shell_screens.hpp"

#include <algorithm>

#include "sc2/app/portrait_catalog.hpp"

namespace sc2::app::shell {

namespace v2 = theme::v2;

namespace {

std::string clean_name(const std::string& raw) {
    std::string out;
    for (char c : raw) {
        const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == ' ' || c == '-' || c == '_' || c == '.';
        if (!ok || (out.empty() && c == ' ')) continue;
        if (out.size() < 24) out.push_back(c);
    }
    return out;
}

const LobbyRace kRaceCells[4] = {LobbyRace::Terran, LobbyRace::Zerg, LobbyRace::Protoss, LobbyRace::Random};

std::string lower(std::string s) {
    for (char& c : s)
        if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
    return s;
}

const char* race_word(char r) { return r == 'T' ? "Terran" : (r == 'Z' ? "Zerg" : (r == 'P' ? "Protoss" : "Neutral")); }

}

bool screen_profile(Ui& ui, ShellCtx& ctx) {
    namespace L = layout::s70_profile;
    chrome(ui, ctx, "PROFILE", kChromeBack | kChromePlate, "C Change portrait   ESC Back");
    ui.fluid({FluidSpan{L::kRecord.x, L::kRecord.x + L::kRecord.w, 1.0f}});

    {
        const Rect frame = ui.at(L::kPortraitFrame170x258);
        Opts o;
        o.trigger = "PortraitButton";
        o.hint = "Choose the picture that stands for you.";
        if (ui.item("profile/portrait", frame, false, "", o)) ctx.router.follow("PortraitButton");
        ui.portrait(frame, ctx.player_portrait());
        ui.publish_region("profile/portrait_frame", frame);
        Opts c = o;
        c.hotkey = 'C';
        if (ui.button("PortraitButton", ui.at(L::kChangePortrait), "Change portrait", ButtonKind::Normal, c)) ctx.router.follow("PortraitButton");
    }

    ui.text(ui.at(L::kLabelName), "NAME", v2::kLabel, v2::kTextMuted);
    {
        std::string name = ctx.settings.get(ShellSettings::kProfileNameKey);
        Opts o;
        o.hint = "Letters, digits, space and - _ . ; up to 24.";
        if (ui.text_field("profile/name", ui.at(L::kNameField), name, 24, "PLAYER", o)) {
            if (!ctx.settings.set_profile_name(clean_name(name))) ctx.fx.toast("Setting could not be saved", true);
        }
    }

    ui.text(ui.at(L::kLabelPreferredRace), "PREFERRED RACE", v2::kLabel, v2::kTextMuted);
    {
        int cell = 0;
        for (int i = 0; i < 4; ++i)
            if (kRaceCells[i] == ctx.settings.profile_race()) cell = i;
        Opts o;
        o.hint = "The race the setup offers you first; it also picks the default portrait.";
        if (ui.segmented("profile/race", ui.at(L::kTerranZergProtossRandom), {"Terran", "Zerg", "Protoss", "Random"}, cell, o)) {
            if (!ctx.settings.set_profile_race(kRaceCells[cell])) ctx.fx.toast("Setting could not be saved", true);
        }
    }

    {
        const Rect card = ui.at(L::kRecord);
        ui.panel(card, "Record");
        const int matches = ctx.settings.get_int(ShellSettings::kProfileMatchesKey, 0);
        const int wins = std::min(matches, ctx.settings.get_int(ShellSettings::kProfileWinsKey, 0));
        const std::string cells[4][2] = {{std::to_string(matches), "MATCHES"},
                                         {std::to_string(wins), "WINS"},
                                         {std::to_string(matches - wins), "LOSSES"},
                                         {matches > 0 ? std::to_string((wins * 100 + matches / 2) / matches) + " %" : std::string("-"), "WIN RATE"}};
        const int pad = ui.px(24.0f), cw = (card.w() - pad * 2) / 4;
        for (int i = 0; i < 4; ++i) {
            const int x = card.x0 + pad + cw * i;
            ui.text(Rect{x, card.y0 + ui.px(88.0f), x + cw, card.y0 + ui.px(144.0f)}, cells[i][0], v2::kStat, v2::kTextHi);
            ui.text(Rect{x, card.y0 + ui.px(160.0f), x + cw, card.y0 + ui.px(188.0f)}, cells[i][1], v2::kLabel, v2::kTextMuted);
        }
        if (matches == 0)
            ui.text(Rect{card.x0 + pad, card.y1 - ui.px(48.0f), card.x1 - pad, card.y1 - ui.px(16.0f)}, "No matches yet. PLAY from the main menu.", v2::kSmall,
                    v2::kTextMuted);
    }
    toasts(ui, ctx);
    return true;
}

bool screen_portraits(Ui& ui, ShellCtx& ctx) {
    namespace L = layout::s71_portraits;
    chrome(ui, ctx, "PORTRAITS", kChromeBack | kChromePlate, "ENTER Select   ESC Back");
    ui.fluid({FluidSpan{L::kTile104x158.x, L::kTile104x158.x + 12 * 113, 1.0f}});
    const int grid_cols = ui.fluid_columns(12, 113.0f, 12 * 113);
    UiState& st = ui.state();
    PortraitCatalog& pc = ctx.portraits();
    if (ui.input()) ctx.scan_portraits(6);

    if (!st.num.count("pick/primed")) {
        st.num["pick/primed"] = 1;
        const PortraitRef cur = PortraitRef::parse(ctx.settings.profile_portrait());
        st.str["pick/sel"] = cur.format();
        st.num["pick/tab"] = cur.kind == PortraitRef::Kind::Hero ? 1 : 0;
        st.num["pick/race"] = 0;
        st.str["pick/search"].clear();
    }
    int& tab = st.num["pick/tab"];
    int& race = st.num["pick/race"];
    std::string& search = st.str["pick/search"];
    std::string& sel = st.str["pick/sel"];

    if (ui.tabs("pick/tabs", ui.at(L::kUnits.x, L::kUnits.y, 488, 64), {"Units", "Heroes"}, tab)) st.scroll_px["pick/grid"] = 0;
    if (ui.segmented("pick/race", ui.at(L::kAllTerranZergProtoss), {"All", "Terran", "Zerg", "Protoss", "Neutral"}, race)) st.scroll_px["pick/grid"] = 0;
    Opts so;
    so.hint = "Type to filter by name.";
    if (ui.text_field("pick/search", ui.at(L::kSearch), search, 24, "Search", so)) st.scroll_px["pick/grid"] = 0;

    const std::vector<PortraitCatalog::Entry>& all = tab == 1 ? pc.heroes() : pc.units();
    static const char kRaceOf[5] = {0, 'T', 'Z', 'P', 'N'};
    const std::string needle = lower(search);
    std::vector<const PortraitCatalog::Entry*> shown;
    for (const PortraitCatalog::Entry& e : all) {
        if (race > 0 && race < 5 && e.race != kRaceOf[race]) continue;
        if (!needle.empty() && lower(e.name).find(needle) == std::string::npos) continue;
        shown.push_back(&e);
    }
    std::string count = std::to_string(shown.size()) + " of " + std::to_string(all.size());
    if (!pc.scanned()) count = "Reading " + std::to_string(pc.scan_done()) + " / " + std::to_string(pc.scan_total());
    ui.text(ui.at(L::kShowingNOfM), count, v2::kSmall, v2::kTextMuted);

    int sel_index = -1;
    for (std::size_t i = 0; i < shown.size(); ++i)
        if (shown[i]->ref.format() == sel) sel_index = int(i);
    bool confirm_now = false;
    const Rect vp = ui.at(L::kTile104x158.x, L::kTile104x158.y, 12 * 113, L::kScrollbar.h);
    if (shown.empty()) {
        const std::string line = !pc.scanned() ? std::string("Reading the portraits...")
                                               : (search.empty() ? std::string("No portraits in this set") : "No portrait matches '" + search + "'");
        ui.text(Rect{vp.x0, vp.y0 + ui.px(160.0f), vp.x1, vp.y0 + ui.px(200.0f)}, line, v2::kH3, v2::kTextHi, Align::Centre);
        if (!search.empty() && ui.button("pick/clear", Rect{(vp.x0 + vp.x1) / 2 - ui.px(160.0f), vp.y0 + ui.px(232.0f), (vp.x0 + vp.x1) / 2 + ui.px(160.0f), vp.y0 + ui.px(296.0f)},
                                        "Clear search", ButtonKind::Normal))
            search.clear();
    } else {
        int moved = sel_index;
        const Ui::Range rg = ui.list("pick/grid", vp, int(shown.size()), ui.px(167.0f), ui.px(158.0f), grid_cols, moved);
        if (moved != sel_index && moved >= 0 && moved < int(shown.size())) {
            sel_index = moved;
            sel = shown[std::size_t(moved)]->ref.format();
        }
        for (int i = rg.first; i < rg.first + rg.count; ++i) {
            const PortraitCatalog::Entry& e = *shown[std::size_t(i)];
            const Rect r = ui.list_item_rect(i);
            Opts o;
            o.hint = e.name + "  -  " + race_word(e.race);
            if (ui.item("pick/" + e.ref.format(), r, i == sel_index, "", o)) {
                sel = e.ref.format();
                sel_index = i;
                confirm_now = ui.double_clicked();
            }
            ui.picture(r.inset(ui.px(4.0f)), ctx.portrait_texture(e.texture), draw::Fit::Contain, 1.0f);
            if (i == sel_index) ui.edge(r, v2::kAccent, 4.0f);
        }
        ui.list_end();
        ui.scrollbar("pick/grid", ui.at(L::kScrollbar));
    }

    const PortraitRef chosen = PortraitRef::parse(sel);
    const IconAtlas::Icon* big = ctx.portrait(chosen);
    ui.panel(ui.at(L::kPreviewPane));
    const Rect frame = ui.at(L::kN170x258);
    ui.portrait(frame, big);
    ui.publish_region("pick/preview", frame);
    std::string name = "Race default", set = "The worker of your race";
    for (const std::vector<PortraitCatalog::Entry>* list : {&pc.units(), &pc.heroes()})
        for (const PortraitCatalog::Entry& e : *list)
            if (e.ref == chosen) {
                name = e.name;
                set = std::string(e.ref.kind == PortraitRef::Kind::Hero ? "Heroes" : "Units") + "  -  " + race_word(e.race);
            }
    ui.text(ui.at(L::kPortraitName), name, v2::kH3, v2::kTextHi, Align::Centre);
    ui.text(ui.at(L::kSet), set, v2::kSmall, v2::kTextMuted, Align::Centre);
    ui.portrait(ui.at(L::kPlate), big);
    ui.portrait(ui.at(L::kList), big);
    ui.portrait(ui.at(L::kSetup), big);

    Opts d;
    d.hotkey = 'W';
    d.hint = "Use the worker of your race as the portrait.";
    if (ui.button("pick/default", ui.at(L::kUseRaceDefault), "Use race default", ButtonKind::Normal, d)) {
        if (!ctx.settings.set_profile_portrait("")) ctx.fx.toast("Setting could not be saved", true);
        ctx.router.cancel();
        return true;
    }
    Opts s;
    s.hint = "Use this portrait.";
    if (ui.button("pick/select", ui.at(L::kSelect), "Select", ButtonKind::Primary, s) || confirm_now) {
        if (!ctx.settings.set_profile_portrait(chosen.format())) ctx.fx.toast("Setting could not be saved", true);
        ctx.router.cancel();
        return true;
    }
    toasts(ui, ctx);
    return true;
}

}
