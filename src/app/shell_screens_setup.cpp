#include "sc2/app/shell_screens.hpp"

#include <algorithm>

namespace sc2::app::shell {

namespace v2 = theme::v2;
namespace L = layout::s20_skirmish_setup;

namespace {

const LobbyRace kRaceCells[4] = {LobbyRace::Terran, LobbyRace::Zerg, LobbyRace::Protoss, LobbyRace::Random};
int race_cell(LobbyRace r) {
    for (int i = 0; i < 4; ++i)
        if (kRaceCells[i] == r) return i;
    return 0;
}

std::string lower(std::string s) {
    for (char& c : s)
        if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
    return s;
}

}

bool screen_setup(Ui& ui, ShellCtx& ctx) {
    chrome(ui, ctx, "SKIRMISH", kChromeBack | kChromePlate, "ENTER Start   ESC Back");
    ui.fluid({FluidSpan{L::kMapListViewport.x, L::kMapListViewport.x + L::kMapListViewport.w, 48.0f / 213.0f},
              FluidSpan{L::kRaceTerranZergProtoss.x, L::kRaceTerranZergProtoss.x + L::kRaceTerranZergProtoss.w, 165.0f / 213.0f}});
    UiState& st = ui.state();
    const std::vector<platform::MapEntry>& maps = ctx.maps();
    if (ui.input()) ctx.warm_map_thumbs(1);

    ui.text(ui.at(L::kLabelMap), "MAP", v2::kLabel, v2::kTextMuted);
    std::string& search = st.str["setup/search"];
    const bool picker = ctx.has_maps_folder_picker();
    {
        Rect sr = ui.at(L::kSearch);
        if (!picker) sr.x1 = ui.at(L::kFolder).x1;
        Opts o;
        o.hint = "Type to filter the maps by name.";
        if (ui.text_field("setup/search", sr, search, 32, "Search", o)) st.scroll_px["setup/maps"] = 0;
        if (picker) {
            Opts f;
            f.hotkey = 'F';
            f.hint = "Point the client at a folder with your own .SC2Map files.";
            if (ui.button("setup/folder", ui.at(L::kFolder), "Folder...", ButtonKind::Normal, f)) ctx.pick_maps_folder();
        }
    }
    std::vector<int> order;
    const std::string needle = lower(search);
    for (std::size_t i = 0; i < maps.size(); ++i)
        if (!maps[i].path.empty() && (needle.empty() || lower(maps[i].name).find(needle) != std::string::npos)) order.push_back(int(i));
    std::sort(order.begin(), order.end(), [&maps](int a, int b) { return lower(maps[std::size_t(a)].name) < lower(maps[std::size_t(b)].name); });
    int real_total = 0;
    for (const platform::MapEntry& e : maps) real_total += !e.path.empty();
    for (std::size_t i = 0; i < maps.size(); ++i)
        if (maps[i].path.empty() && (needle.empty() || std::string("test arena (built-in)").find(needle) != std::string::npos)) order.push_back(int(i));

    const int selected = ctx.selected_map();
    int sel_row = -1;
    for (std::size_t i = 0; i < order.size(); ++i)
        if (order[i] == selected) sel_row = int(i);
    const Rect vp = ui.at(L::kMapListViewport);
    const int pitch = ui.px(68.0f), row_h = ui.px(64.0f);
    if (!st.num.count("setup/primed")) {
        st.num["setup/primed"] = 1;
        if (sel_row >= 0) st.scroll_px["setup/maps"] = std::max(0, sel_row * pitch - (vp.h() - row_h) / 2);
    }
    bool start_now = false;
    if (order.empty()) {
        const bool none = real_total == 0 && search.empty();
        ui.fill(vp, v2::kSurface1);
        ui.text(Rect{vp.x0, vp.y0 + ui.px(120.0f), vp.x1, vp.y0 + ui.px(160.0f)}, none ? std::string("No maps found") : "No map matches '" + search + "'", v2::kH3,
                v2::kTextHi, Align::Centre);
        if (none) {
            ui.text(Rect{vp.x0 + ui.px(24.0f), vp.y0 + ui.px(176.0f), vp.x1 - ui.px(24.0f), vp.y0 + ui.px(206.0f)}, "The game folder has no melee maps this client can read.",
                    v2::kSmall, v2::kTextBody, Align::Centre);
            ui.text(Rect{vp.x0 + ui.px(24.0f), vp.y0 + ui.px(206.0f), vp.x1 - ui.px(24.0f), vp.y0 + ui.px(236.0f)}, "Point at a folder with .SC2Map files.", v2::kSmall,
                    v2::kTextBody, Align::Centre);
        } else if (ui.button("setup/clear", Rect{(vp.x0 + vp.x1) / 2 - ui.px(140.0f), vp.y0 + ui.px(192.0f), (vp.x0 + vp.x1) / 2 + ui.px(140.0f), vp.y0 + ui.px(256.0f)},
                             "Clear search", ButtonKind::Normal)) {
            search.clear();
        }
    } else {
        int moved = sel_row;
        const Ui::Range rg = ui.list("setup/maps", vp, int(order.size()), pitch, row_h, 1, moved);
        if (moved != sel_row && moved >= 0 && moved < int(order.size())) {
            sel_row = moved;
            ctx.select_map(order[std::size_t(moved)]);
        }
        for (int i = rg.first; i < rg.first + rg.count; ++i) {
            const int mi = order[std::size_t(i)];
            const platform::MapEntry& e = maps[std::size_t(mi)];
            const bool arena = e.path.empty();
            const Rect r = ui.list_item_rect(i);
            Opts o;
            o.hint = arena ? "The built-in flat arena: 72 x 72, no cliffs." : (e.casc ? "An install map, read from the game data." : e.path);
            const int thumb = ui.px(56.0f), pad = ui.px(4.0f);
            if (ui.item(arena ? std::string("TEST ARENA (built-in)") : e.name, r, i == sel_row, "", o)) {
                ctx.select_map(mi);
                sel_row = i;
                start_now = ui.double_clicked();
            }
            const Rect tr{r.x0 + pad * 2, r.y0 + pad, r.x0 + pad * 2 + thumb, r.y0 + pad + thumb};
            ui.image(tr, ctx.map_thumb(mi), draw::Fit::Contain, "", 1.0f);
            ui.text(Rect{tr.x1 + ui.px(16.0f), r.y0, r.x1 - ui.px(16.0f), r.y1}, arena ? std::string("TEST ARENA (built-in)") : e.name, v2::kBody,
                    i == sel_row ? v2::kTextHi : v2::kTextBody);
        }
        ui.list_end();
        ui.scrollbar("setup/maps", ui.at(L::kScrollbar));
    }
    {
        std::string count = std::to_string(real_total) + " maps";
        if (!search.empty()) count = std::to_string(order.size()) + " of " + std::to_string(real_total + 1);
        ui.text(ui.at(L::kCount43MapsReading), count, v2::kSmall, v2::kTextMuted);
    }

    ui.text(ui.at(L::kLabelPlayers), "PLAYERS", v2::kLabel, v2::kTextMuted);
    {
        ui.panel(ui.at(L::kSlotCardYou));
        theme::Rgb tc{};
        const bool have_tc = ctx.team_color(0, tc);
        ui.portrait(ui.at(L::kPortraitFrame126x192), ctx.player_portrait(), have_tc ? &tc : nullptr);
        ui.publish_region("setup/you_portrait", ui.at(L::kPortraitFrame126x192));
        const Rect nr = ui.at(L::kNameTeamColourChip);
        ui.text(Rect{nr.x0, nr.y0, nr.x1 - ui.px(40.0f), nr.y1}, ctx.player_name(), v2::kH3, v2::kTextHi);
        if (have_tc) ui.fill(Rect{nr.x1 - ui.px(24.0f), nr.y0 + ui.px(6.0f), nr.x1, nr.y0 + ui.px(30.0f)}, tc);
        ui.text(Rect{nr.x0, nr.y1 + ui.px(8.0f), nr.x1, nr.y1 + ui.px(36.0f)}, "YOU", v2::kLabel, v2::kTextMuted);
        int cell = race_cell(ctx.settings.profile_race());
        Opts o;
        o.hint = "Your race. Random is drawn at START, from the match's own seed.";
        if (ui.segmented("setup/race", ui.at(L::kRaceTerranZergProtoss), {"Terran", "Zerg", "Protoss", "Random"}, cell, o) &&
            !ctx.settings.set_profile_race(kRaceCells[cell]))
            ctx.fx.toast("Setting could not be saved", true);
    }
    ui.text(ui.at(L::kVs), "VS", v2::kLabel, v2::kTextFaint, Align::Centre);
    {
        ui.panel(ui.at(L::kSlotCardOpponent));
        theme::Rgb tc{};
        const bool have_tc = ctx.team_color(1, tc);
        const Rect af = ui.at(L::kAIAvatarFrame);
        ui.image(af, ctx.ai_avatar(), draw::Fit::Contain, "A.I.", 1.5f);
        if (have_tc) ui.fill(Rect{af.x0, af.y1, af.x1, af.y1 + ui.px(6.0f)}, tc);
        const Rect nr = ui.at(L::kAITeamColour);
        ui.text(Rect{nr.x0, nr.y0, nr.x1 - ui.px(40.0f), nr.y1}, "A.I.", v2::kH3, v2::kTextHi);
        if (have_tc) ui.fill(Rect{nr.x1 - ui.px(24.0f), nr.y0 + ui.px(4.0f), nr.x1, nr.y0 + ui.px(28.0f)}, tc);
        int cell = race_cell(ctx.settings.opponent_race());
        Opts o;
        o.hint = "The computer's race.";
        if (ui.segmented("setup/ai_race", ui.at(L::kRaceTZP), {"Terran", "Zerg", "Protoss", "Random"}, cell, o) && !ctx.settings.set_opponent_race(kRaceCells[cell]))
            ctx.fx.toast("Setting could not be saved", true);
        int tier = ctx.settings.ai_difficulty();
        Opts d;
        d.hint = "Very Easy .. Elite: the game's own A.I. tiers.";
        const Rect minus = ui.at(L::kMinus), plus = ui.at(L::kPlus);
        if (ui.stepper("setup/difficulty", Rect{minus.x0, minus.y0, plus.x1, plus.y1}, tier, 1, 7, ShellSettings::difficulty_name(tier), d) &&
            !ctx.settings.set(ShellSettings::kAiDifficultyKey, std::to_string(tier)))
            ctx.fx.toast("Setting could not be saved", true);
    }
    const std::string& refusal = ctx.match_error();
    if (!refusal.empty()) {
        ui.banner(ui.at(L::kErrorNoticeBanner), State::Err, "Refused", refusal);
        ui.publish_region("setup/banner", ui.at(L::kErrorNoticeBanner), refusal);
    }

    ui.text(ui.at(L::kLabelMatch), "MATCH", v2::kLabel, v2::kTextMuted);
    const bool have_sel = selected >= 0 && selected < int(maps.size());
    const bool sel_arena = have_sel && maps[std::size_t(selected)].path.empty();
    const std::string sel_name = !have_sel ? std::string() : (sel_arena ? std::string("TEST ARENA (built-in)") : maps[std::size_t(selected)].name);
    ui.image(ui.at(L::kMapPreviewWell), have_sel ? ctx.map_preview(selected) : nullptr, draw::Fit::Contain,
             !have_sel ? "No map selected" : (sel_arena ? "The built-in arena has no picture" : "No preview in this map"), 2.0f);
    ui.text(ui.at(L::kMapName), sel_name, v2::kH3, v2::kTextHi);
    if (have_sel)
        ui.text(ui.at(L::kSize), sel_arena ? std::string("72 x 72 - 2 starts - built-in") : (maps[std::size_t(selected)].casc ? std::string("INSTALL") : maps[std::size_t(selected)].path),
                v2::kSmall, v2::kTextMuted);
    ui.text(ui.at(L::kLabelGameSpeed), "GAME SPEED", v2::kLabel, v2::kTextMuted);
    {
        int sp = int(ctx.settings.game_speed());
        Opts o;
        o.hint = "How fast a match runs. Ladder melee runs at Faster.";
        if (ui.segmented("setup/speed", ui.at(L::kSlowerSlowNormalFast), {"Slower", "Slow", "Normal", "Fast", "Faster"}, sp, o) &&
            !ctx.settings.set(ShellSettings::kGameSpeedKey, std::to_string(sp)))
            ctx.fx.toast("Setting could not be saved", true);
    }
    {
        const Rect card = ui.at(L::kSummaryMatchup);
        ui.panel(card);
        const int pad = ui.px(24.0f);
        const std::string matchup = std::string(lobby_race_name(ctx.settings.profile_race())) + " vs " + lobby_race_name(ctx.settings.opponent_race()) + " A.I. (" +
                                    ShellSettings::difficulty_name(ctx.settings.ai_difficulty()) + ")";
        ui.text(Rect{card.x0 + pad, card.y0 + ui.px(16.0f), card.x1 - pad, card.y0 + ui.px(48.0f)}, matchup, v2::kBody, v2::kTextHi);
        ui.text(Rect{card.x0 + pad, card.y0 + ui.px(50.0f), card.x1 - pad, card.y0 + ui.px(80.0f)},
                std::string("1v1 MELEE - ") + ShellSettings::speed_name(ctx.settings.game_speed()), v2::kSmall, v2::kTextMuted);
    }
    {
        Opts o;
        o.enabled = have_sel;
        o.hint = "Load the match.";
        o.why_disabled = "Pick a map first.";
        if (!have_sel) ui.text(ui.at(L::kWhyStartIsDisabled), "Pick a map first.", v2::kSmall, v2::kWarn);
        if ((ui.button("START GAME", ui.at(L::kStartGame), "Start game", ButtonKind::Primary, o) || (start_now && have_sel)) && !ctx.start_selected_match("StartButton -> countdown"))
            ctx.fx.toast("The match could not be started.", true);
    }
    toasts(ui, ctx);
    return true;
}

}
