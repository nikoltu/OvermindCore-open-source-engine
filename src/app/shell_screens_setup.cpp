#include "omc/app/shell_screens.hpp"

#include <algorithm>

namespace omc::app::shell {

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

constexpr layout::R kFormatLabel{736, 808, 300, 24};
constexpr layout::R kFormat{736, 840, 741, 64};
constexpr float kRowsTop = 160.0f, kRowsHead = 32.0f, kRowPitch = 64.0f, kRowH = 56.0f;
constexpr float kColChip = 736.0f, kColCtl = 760.0f, kColRace = 968.0f, kColDiff = 1146.0f, kColTeam = 1354.0f, kColEnd = 1477.0f;

const char* race_word(LobbyRace r) {
    return r == LobbyRace::Protoss ? "Protoss" : (r == LobbyRace::Zerg ? "Zerg" : (r == LobbyRace::Random ? "Random" : "Terran"));
}

}

void slot_rows(Ui& ui, ShellCtx& ctx, bool custom) {
    const int rows = ctx.lobby_rows();
    const float hy = kRowsTop;
    ui.text(ui.at(kColCtl, hy, kColRace - kColCtl - 8.0f, 24.0f), "PLAYER", v2::kLabel, v2::kTextMuted);
    ui.text(ui.at(kColRace, hy, kColDiff - kColRace - 8.0f, 24.0f), "RACE", v2::kLabel, v2::kTextMuted);
    ui.text(ui.at(kColDiff, hy, kColTeam - kColDiff - 8.0f, 24.0f), "DIFFICULTY", v2::kLabel, v2::kTextMuted);
    ui.text(ui.at(kColTeam, hy, kColEnd - kColTeam, 24.0f), "TEAM", v2::kLabel, v2::kTextMuted);
    static const std::vector<std::string> kCtl = {"Human", "A.I.", "Closed"};
    static const std::vector<std::string> kCustomCtl = {"You", "A.I.", "Open", "Closed"};
    static const int kCustomToCtl[4] = {0, 1, 3, 2};
    static const int kCtlToCustom[4] = {0, 1, 3, 2};
    static const std::vector<std::string> kRace = {"Terran", "Zerg", "Protoss", "Random"};
    static const std::vector<std::string> kTeam = {"-", "Team 1", "Team 2", "Team 3", "Team 4"};
    std::vector<std::string> diffs;
    for (int t = 1; t <= 7; ++t) diffs.push_back(ShellSettings::difficulty_name(t));
    int player = 0;
    for (int i = 0; i < rows; ++i) {
        int ctl = 2, race = 0, diff = 0, team = -1;
        ctx.lobby_row(i, ctl, race, diff, team);
        const float y = kRowsTop + kRowsHead + kRowPitch * float(i);
        const std::string id = std::string(custom ? "custom/slot" : "setup/slot") + std::to_string(i);
        const bool open = ctl == 0 || ctl == 1, ai = ctl == 1;
        theme::Rgb tc{};
        if (open && ctx.team_color(player, tc)) ui.fill(ui.at(kColChip, y + 8.0f, 12.0f, kRowH - 16.0f), tc);
        if (open) ++player;
        int c = custom ? kCtlToCustom[std::max(0, std::min(3, ctl))] : ctl, r = race_cell(LobbyRace(race)), d = std::max(1, diff) - 1, t = team + 1;
        Opts oc;
        oc.hint = custom ? "You: your seat. A.I.: the computer. Open: free for a LAN player, empty when an offline game starts. Closed: nobody plays this start."
                         : "Human: you. A.I.: the computer. Closed: nobody plays this start.";
        bool changed = ui.dropdown(id + "/controller", ui.at(kColCtl, y, kColRace - kColCtl - 8.0f, kRowH), custom ? kCustomCtl : kCtl, c, oc);
        Opts orc;
        orc.enabled = open;
        orc.hint = "This player's race. Random is drawn at START, from the match's own seed.";
        changed = ui.dropdown(id + "/race", ui.at(kColRace, y, kColDiff - kColRace - 8.0f, kRowH), kRace, r, orc) || changed;
        Opts od;
        od.enabled = ai;
        od.hint = "This A.I.'s tier, Very Easy .. Elite.";
        if (ai) changed = ui.dropdown(id + "/difficulty", ui.at(kColDiff, y, kColTeam - kColDiff - 8.0f, kRowH), diffs, d, od) || changed;
        else ui.text(ui.at(kColDiff + 16.0f, y, kColTeam - kColDiff - 24.0f, kRowH), ctl == 0 ? ctx.player_name() : (ctl == 3 ? std::string("Open") : std::string()), v2::kBody,
                     v2::kTextMuted);
        Opts ot;
        ot.enabled = open;
        ot.hint = "Players on one team are allies: shared vision, and they win together. '-' plays alone.";
        changed = ui.dropdown(id + "/team", ui.at(kColTeam, y, kColEnd - kColTeam, kRowH), kTeam, t, ot) || changed;
        const int pick = custom ? kCustomToCtl[std::max(0, std::min(3, c))] : c;
        if (changed) ctx.set_lobby_row(i, pick, int(kRaceCells[std::max(0, std::min(3, r))]), pick == 1 ? d + 1 : 0, t - 1);
    }
}

std::string team_matchup(const ShellCtx& ctx) {
    std::vector<std::pair<int, std::string>> sides;
    for (int i = 0; i < ctx.lobby_rows(); ++i) {
        int ctl = 2, race = 0, diff = 0, team = -1;
        ctx.lobby_row(i, ctl, race, diff, team);
        if (ctl != 0 && ctl != 1) continue;
        const int key = team >= 0 ? team : -(i + 2);
        const std::string who = std::string(ctl == 0 ? "You" : "A.I.") + " (" + race_word(LobbyRace(race)) + ")";
        bool found = false;
        for (auto& s : sides)
            if (s.first == key) {
                s.second += " + " + who;
                found = true;
            }
        if (!found) sides.emplace_back(key, who);
    }
    std::string out;
    for (std::size_t s = 0; s < sides.size(); ++s) out += (s ? " vs " : "") + sides[s].second;
    return out.empty() ? std::string("Nobody plays - open a row") : out;
}

bool map_picker(Ui& ui, ShellCtx& ctx, const std::string& key) {
    namespace L = layout::s20_skirmish_setup;
    UiState& st = ui.state();
    const std::vector<platform::MapEntry>& maps = ctx.maps();
    if (ui.input()) ctx.warm_map_thumbs(1);
    bool start_now = false;
    ui.text(ui.at(L::kLabelMap), "MAP", v2::kLabel, v2::kTextMuted);
    std::string& search = st.str[key + "/search"];
    const bool picker = ctx.has_maps_folder_picker();
    {
        Rect sr = ui.at(L::kSearch);
        if (!picker) sr.x1 = ui.at(L::kFolder).x1;
        Opts o;
        o.hint = "Type to filter the maps by name.";
        if (ui.text_field(key + "/search", sr, search, 32, "Search", o)) st.scroll_px[key + "/maps"] = 0;
        if (picker) {
            Opts f;
            f.hotkey = 'F';
            f.hint = "Point the client at a folder with your own .SC2Map files.";
            if (ui.button(key + "/folder", ui.at(L::kFolder), "Folder...", ButtonKind::Normal, f)) ctx.pick_maps_folder();
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
        if (maps[i].path.empty() && ctx.has_arena() && (needle.empty() || std::string("test arena (built-in)").find(needle) != std::string::npos)) order.push_back(int(i));

    const int selected = ctx.selected_map();
    int sel_row = -1;
    for (std::size_t i = 0; i < order.size(); ++i)
        if (order[i] == selected) sel_row = int(i);
    const Rect vp = ui.at(L::kMapListViewport);
    const int pitch = ui.px(68.0f), row_h = ui.px(64.0f);
    if (!st.num.count(key + "/primed")) {
        st.num[key + "/primed"] = 1;
        if (sel_row >= 0) st.scroll_px[key + "/maps"] = std::max(0, sel_row * pitch - (vp.h() - row_h) / 2);
    }
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
        } else if (ui.button(key + "/clear", Rect{(vp.x0 + vp.x1) / 2 - ui.px(140.0f), vp.y0 + ui.px(192.0f), (vp.x0 + vp.x1) / 2 + ui.px(140.0f), vp.y0 + ui.px(256.0f)},
                             "Clear search", ButtonKind::Normal)) {
            search.clear();
        }
    } else {
        int moved = sel_row;
        const Ui::Range rg = ui.list(key + "/maps", vp, int(order.size()), pitch, row_h, 1, moved);
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
        ui.scrollbar(key + "/maps", ui.at(L::kScrollbar));
    }
    {
        std::string count = std::to_string(real_total) + " maps";
        if (!search.empty()) count = std::to_string(order.size()) + " of " + std::to_string(real_total + 1);
        ui.text(ui.at(L::kCount43MapsReading), count, v2::kSmall, v2::kTextMuted);
    }
    return start_now;
}

bool screen_setup(Ui& ui, ShellCtx& ctx) {
    chrome(ui, ctx, "SKIRMISH", kChromeBack | kChromePlate, "ENTER Start   ESC Back");
    ui.fluid({FluidSpan{L::kMapListViewport.x, L::kMapListViewport.x + L::kMapListViewport.w, 48.0f / 213.0f},
              FluidSpan{L::kRaceTerranZergProtoss.x, L::kRaceTerranZergProtoss.x + L::kRaceTerranZergProtoss.w, 165.0f / 213.0f}});
    const bool start_now = map_picker(ui, ctx, "setup");
    const std::vector<platform::MapEntry>& maps = ctx.maps();
    const int selected = ctx.selected_map();

    ui.text(ui.at(L::kLabelPlayers), "PLAYERS", v2::kLabel, v2::kTextMuted);
    const int starts = ctx.map_start_count(selected);
    {
        ui.text(ui.at(kFormatLabel), "FORMAT", v2::kLabel, v2::kTextMuted);
        ui.text(ui.at(kFormatLabel.x + 160.0f, kFormatLabel.y, kFormatLabel.w + 281.0f, kFormatLabel.h),
                std::to_string(starts) + " start locations on this map", v2::kSmall, v2::kTextFaint, Align::Right);
        int fmt = ctx.lobby_format();
        Opts o;
        o.hint = "1v1, a team game or a free-for-all. A format needs as many start locations as it has players.";
        if (ui.segmented("setup/format", ui.at(kFormat), {"1v1", "2v2", "3v3", "4v4", "FFA"}, fmt, o) && !ctx.set_lobby_format(fmt))
            ctx.fx.toast(std::string(fmt == 4 ? "FFA" : std::to_string(fmt + 1) + "v" + std::to_string(fmt + 1)) + " needs " +
                             std::to_string(fmt == 4 ? 3 : 2 * (fmt + 1)) + " start locations - this map has " + std::to_string(starts),
                         true);
    }
    const bool team_mode = ctx.lobby_format() != 0 && ctx.lobby_rows() > 0;
    if (team_mode) slot_rows(ui, ctx, false);
    if (!team_mode) {
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
    if (!team_mode) ui.text(ui.at(L::kVs), "VS", v2::kLabel, v2::kTextFaint, Align::Centre);
    if (!team_mode) {
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
        ui.text(ui.at(L::kSize), sel_arena ? std::string("72 x 72 - 2 mains + 2 naturals - built-in") : (maps[std::size_t(selected)].casc ? std::string("INSTALL") : maps[std::size_t(selected)].path),
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
        const std::string matchup = team_mode ? team_matchup(ctx)
                                              : std::string(lobby_race_name(ctx.settings.profile_race())) + " vs " + lobby_race_name(ctx.settings.opponent_race()) +
                                                    " A.I. (" + ShellSettings::difficulty_name(ctx.settings.ai_difficulty()) + ")";
        ui.text(Rect{card.x0 + pad, card.y0 + ui.px(16.0f), card.x1 - pad, card.y0 + ui.px(48.0f)}, matchup, v2::kBody, v2::kTextHi);
        static const char* kFormatWords[5] = {"1v1", "2v2", "3v3", "4v4", "FFA"};
        ui.text(Rect{card.x0 + pad, card.y0 + ui.px(50.0f), card.x1 - pad, card.y0 + ui.px(80.0f)},
                std::string(kFormatWords[std::max(0, std::min(4, ctx.lobby_format()))]) + " MELEE - " + ShellSettings::speed_name(ctx.settings.game_speed()), v2::kSmall,
                v2::kTextMuted);
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
