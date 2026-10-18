#include "sc2/app/shell_screens.hpp"

#include <cstdio>
#include <filesystem>

namespace sc2::app::shell {

namespace v2 = theme::v2;
namespace L = layout::s10_home;

namespace {

const char* race_word(Race r) { return r == Race::Protoss ? "Protoss" : (r == Race::Zerg ? "Zerg" : "Terran"); }

std::string clock_text(long long ticks, int ticks_per_second) {
    const long long s = ticks_per_second > 0 ? ticks / ticks_per_second : 0;
    char buf[32];
    std::snprintf(buf, sizeof buf, "%lld:%02lld", s / 60, s % 60);
    return buf;
}

std::string stamp_text(const std::string& t) {
    if (t.size() < 13 || t[8] != '-') return t;
    return t.substr(0, 4) + "-" + t.substr(4, 2) + "-" + t.substr(6, 2) + " " + t.substr(9, 2) + ":" + t.substr(11, 2);
}

}

bool screen_home(Ui& ui, ShellCtx& ctx) {
    chrome(ui, ctx, "", kChromeHome | kChromePlate, "ENTER Play   ESC Quit");
    ui.fluid({FluidSpan{L::kPlay.x, L::kPlay.x + L::kPlay.w, 1.0f}});

    const std::vector<platform::MapEntry>& maps = ctx.maps();
    const int map_index = ctx.setup_map();
    const bool have_map = map_index >= 0;
    const std::string map_name = have_map ? ctx.map_title(map_index) : std::string();
    const std::string matchup = std::string(lobby_race_name(ctx.settings.profile_race())) + " vs " + lobby_race_name(ctx.settings.opponent_race()) + " A.I. (" +
                                ShellSettings::difficulty_name(ctx.settings.ai_difficulty()) + ")";
    const std::string speed = ShellSettings::speed_name(ctx.settings.game_speed());

    {
        Opts o;
        o.trigger = "PlayNowButton";
        o.enabled = have_map;
        o.hint = "Start a 1v1 against the A.I. with your last setup.";
        o.why_disabled = "No maps found. Add a maps folder in the skirmish setup.";
        const Rect r = ui.at(L::kPlay);
        if (ui.button("PlayCard", r, "", ButtonKind::Primary, o)) {
            if (!ctx.start_match("PlayNowButton")) {
                ctx.router.follow("PlayButton");
                ctx.fx.toast(ctx.stored_map_label() + " is no longer available. Pick another map.", true);
            }
        }
        const theme::Rgb hi = have_map ? v2::kTextHi : v2::kTextDisabled;
        const int thumb = ui.px(112.0f), inset = ui.px(24.0f), pad = ui.px(32.0f);
        const Rect tr{r.x1 - inset - thumb, r.y0 + inset, r.x1 - inset, r.y0 + inset + thumb};
        const int text_x1 = tr.x0 - inset;
        ui.text(Rect{r.x0 + pad, r.y0 + ui.px(28.0f), text_x1, r.y0 + ui.px(84.0f)}, "PLAY", v2::kButtonXl, hi);
        ui.text(Rect{r.x0 + pad, r.y0 + ui.px(92.0f), text_x1, r.y0 + ui.px(120.0f)}, have_map ? matchup : std::string("No maps found"), v2::kBody, hi);
        if (have_map) {
            ui.text(Rect{r.x0 + pad, r.y0 + ui.px(124.0f), text_x1, r.y0 + ui.px(150.0f)}, map_name + " - " + speed, v2::kSmall, v2::kTextBody);
            ui.image(tr, ctx.map_preview(map_index), draw::Fit::Contain, "", 2.0f);
        }
    }

    struct Row {
        const layout::R& rect;
        const char* id;
        const char* label;
        const char* trigger;
        char hotkey;
        const char* hint;
        bool enabled;
    };
    const Row rows[] = {
        {L::kSkirmishSetup, "SkirmishSetupRow", "Skirmish setup", "PlayButton", 'S', "Pick the map, the races, the A.I. tier and the speed.", true},
        {L::kReplaysAndSaves, "ReplaysRow", "Replays and saves", "ReplayButton", 'R', "Watch a recorded match or resume a saved one.", true},
        {L::kProfile, "ProfileRow", "Profile", "ProfileButton", 'P', "Your name, race, portrait and record.", true},
        {L::kSettings, "SettingsRow", "Settings", "menu/Options", 'O', "Gameplay, video, audio and controls.", true},
        {L::kLanGame, "LanGameRow", "LAN game", "", 0, "", false},
        {L::kCampaign, "CampaignRow", "Campaign", "", 0, "", false},
        {L::kQuit, "Quit game", "Quit", "", 'Q', "Leave the client (asks first).", true},
    };
    for (const Row& row : rows) {
        Opts o;
        o.trigger = row.trigger;
        o.hotkey = row.hotkey;
        o.hint = row.hint;
        o.enabled = row.enabled;
        if (!row.enabled) {
            o.tag = "SOON";
            o.why_disabled = "Not in this build yet.";
        }
        if (!ui.button(row.id, ui.at(row.rect), row.label, ButtonKind::Menu, o)) continue;
        if (row.trigger[0]) ctx.router.follow(row.trigger);
        else ui.open_modal("quit");
    }
    {
        Opts o;
        o.trigger = "SandboxButton";
        o.hint = "One race, no opponent: spawn any unit and watch it.";
        if (ui.button("SandboxButton", ui.at(L::kSandbox), "Sandbox", ButtonKind::Ghost, o)) ctx.router.follow("SandboxButton");
        Opts a;
        a.trigger = "Credits entry";
        a.hint = "What this is, which build, licences.";
        if (ui.button("AboutButton", ui.at(L::kAbout), "About", ButtonKind::Ghost, a)) ctx.router.follow("Credits entry");
    }

    {
        const ShellSettings::LastMatch last = ctx.settings.last_match();
        const Rect card = ui.at(L::kLastMatchCard);
        ui.panel(card, "Last match");
        const Rect tr = ui.at(L::kVerdict);
        const int lh = ui.px(40.0f);
        const int top = tr.y0 + ui.px(24.0f);
        if (!last.valid()) {
            ui.text(Rect{card.x0 + ui.px(24.0f), top, card.x1 - ui.px(24.0f), top + lh}, "No matches yet", v2::kH3, v2::kTextHi);
            ui.text(Rect{card.x0 + ui.px(24.0f), top + lh, card.x1 - ui.px(24.0f), top + lh * 2},
                    have_map ? "PLAY starts a 1v1 against the A.I. on " + map_name + "." : std::string("Add a maps folder in the skirmish setup to play."), v2::kBody,
                    v2::kTextBody);
        } else {
            const bool won = last.outcome == "victory";
            ui.image(ui.at(L::kMapThumb), ctx.map_preview(ctx.map_by_title(last.map)), draw::Fit::Contain, "No preview", 2.0f);
            ui.text(Rect{tr.x0, top, tr.x1, top + lh}, std::string(won ? "VICTORY" : "DEFEAT") + "  -  " + last.map, v2::kH2, won ? v2::kOk : v2::kTextMuted);
            std::string line = std::string(race_word(Race(last.race[0]))) + " vs " + race_word(Race(last.race[1])) + " A.I.";
            if (last.tier > 0) line += std::string(" (") + ShellSettings::difficulty_name(last.tier) + ")";
            line += " - " + clock_text(last.ticks, ctx.ticks_per_second());
            ui.text(Rect{tr.x0, top + lh, tr.x1, top + lh * 2}, line, v2::kBody, v2::kTextBody);
            ui.text(Rect{tr.x0, top + lh * 2, tr.x1, top + lh * 2 + ui.px(28.0f)}, stamp_text(last.time), v2::kSmall, v2::kTextMuted);
            std::error_code ec;
            if (!last.replay.empty() && std::filesystem::exists(last.replay, ec)) {
                Opts o;
                o.trigger = "WatchReplayButton";
                o.hotkey = 'W';
                o.hint = "Watch the recording of your last match.";
                if (ui.button("WatchReplayButton", ui.at(L::kWatchReplay), "Watch replay", ButtonKind::Normal, o) && !ctx.watch_replay(last.replay, "WatchReplayButton"))
                    ctx.fx.toast("That recording could not be read.", true);
            }
        }
    }

    {
        const Rect card = ui.at(L::kRecordCard);
        ui.panel(card, "Record");
        const int matches = ctx.settings.get_int(ShellSettings::kProfileMatchesKey, 0);
        const int wins = ctx.settings.get_int(ShellSettings::kProfileWinsKey, 0);
        const std::string cells[3][2] = {{std::to_string(matches), "MATCHES"},
                                         {std::to_string(wins), "WINS"},
                                         {matches > 0 ? std::to_string((wins * 100 + matches / 2) / matches) + " %" : std::string("-"), "WIN RATE"}};
        const int pad = ui.px(24.0f), cw = (card.w() - pad * 2) / 3;
        for (int i = 0; i < 3; ++i) {
            const int x = card.x0 + pad + cw * i;
            ui.text(Rect{x, card.y0 + ui.px(72.0f), x + cw, card.y0 + ui.px(128.0f)}, cells[i][0], v2::kStat, v2::kTextHi);
            ui.text(Rect{x, card.y0 + ui.px(140.0f), x + cw, card.y0 + ui.px(168.0f)}, cells[i][1], v2::kLabel, v2::kTextMuted);
        }
    }

    {
        const Rect card = ui.at(L::kGameDataCard);
        ui.panel(card, "Game data");
        const ShellCtx::GameData gd = ctx.game_data();
        const int pad = ui.px(24.0f);
        ui.text(Rect{card.x0 + pad, card.y0 + ui.px(72.0f), card.x1 - pad, card.y0 + ui.px(104.0f)}, gd.version.empty() ? std::string("No game folder reported") : gd.version,
                v2::kBody, v2::kTextBody);
        const char* word = !gd.valid ? "NOT CHECKED" : (gd.verified ? "VERIFIED" : "UNVERIFIED BUILD");
        ui.text(Rect{card.x0 + pad, card.y0 + ui.px(112.0f), card.x1 - pad, card.y0 + ui.px(140.0f)}, word, v2::kLabel,
                !gd.valid ? v2::kTextMuted : (gd.verified ? v2::kOk : v2::kWarn));
        ui.text(Rect{card.x0 + pad, card.y0 + ui.px(148.0f), card.x1 - pad, card.y0 + ui.px(176.0f)},
                std::to_string(maps.size() > 0 ? maps.size() - 1 : 0) + " maps", v2::kSmall, v2::kTextMuted);
    }

    if (!ctx.match_error().empty()) ui.banner(ui.at(L::kNoticeBanner), State::Err, "Refused", ctx.match_error());

    if (ui.escape_pending() && ui.state().modal.empty()) {
        ui.use_escape();
        ui.open_modal("quit");
    }
    if (confirm(ui, "quit", "Quit Crafter?", "The client closes. Settings are already saved.", "Stay", "Quit") == Confirm::Yes) ctx.quit();
    toasts(ui, ctx);
    return true;
}

}
