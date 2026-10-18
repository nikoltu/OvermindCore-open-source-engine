#include "sc2/app/shell_screens.hpp"

#include <algorithm>
#include <cstdio>
#include <filesystem>

#include "sc2/app/front_end.hpp"

namespace sc2::app::shell {

namespace v2 = theme::v2;
namespace L = layout::s50_result;

namespace {
const char* race_word(Race r) { return r == Race::Protoss ? "Protoss" : (r == Race::Zerg ? "Zerg" : "Terran"); }
}

bool screen_score(Ui&, ShellCtx&) { return false; }

bool screen_result(Ui& ui, ShellCtx& ctx) {
    const ShellCtx::ResultView v = ctx.result();
    if (!v.decided) return false;
    chrome(ui, ctx, "", kChromeBare, "ENTER Play again   ESC Main menu");
    ui.fluid_centred(L::kPlayAgain.x, L::kMainMenu.x + L::kMainMenu.w);

    const Rect vr = ui.at(L::kDisplay);
    ui.text(vr, v.victory ? "VICTORY" : "DEFEAT", v2::kDisplay, v.victory ? v2::kTextHi : v2::kTextMuted, Align::Centre);
    const int rule_w = ui.px(480.0f);
    ui.fill(Rect{(vr.x0 + vr.x1 - rule_w) / 2, vr.y1 + ui.px(4.0f), (vr.x0 + vr.x1 + rule_w) / 2, vr.y1 + ui.px(8.0f)}, v.victory ? v2::kAccent : v2::kLine3);
    {
        const long long s = ctx.ticks_per_second() > 0 ? v.ticks / ctx.ticks_per_second() : 0;
        char clock[32];
        std::snprintf(clock, sizeof clock, "%lld:%02lld", s / 60, s % 60);
        ui.text(ui.at(L::kMap), v.map + "  -  " + clock + "  -  " + ShellSettings::speed_name(ctx.settings.game_speed()), v2::kBody, v2::kTextMuted, Align::Centre);
    }

    theme::Rgb tc0{}, tc1{};
    const bool have0 = ctx.team_color(0, tc0), have1 = ctx.team_color(1, tc1);
    {
        const Rect card = ui.at(L::kYourCard);
        ui.panel(card);
        if (v.victory) ui.edge(card, v2::kAccent);
        const Rect frame = v.victory ? ui.at(L::kPortrait170x258) : ui.at(L::kPortrait170x258.x + 22, L::kPortrait170x258.y + 33, 125, 191);
        ui.portrait(frame, ctx.player_portrait(), have0 ? &tc0 : nullptr);
        ui.publish_region("result/you_portrait", frame);
        const Rect nr = ui.at(L::kName);
        ui.text(nr, ctx.player_name(), v2::kH3, v.victory ? v2::kTextHi : v2::kTextMuted);
        ui.text(Rect{nr.x0, nr.y1 + ui.px(12.0f), nr.x1, nr.y1 + ui.px(40.0f)}, race_word(v.race[0]), v2::kBody, v2::kTextBody);
        if (v.victory) ui.text(Rect{nr.x0, nr.y1 + ui.px(56.0f), nr.x1, nr.y1 + ui.px(80.0f)}, "WINNER", v2::kLabel, v2::kOk);
    }
    {
        const Rect card = ui.at(L::kOpponentCard);
        ui.panel(card);
        if (!v.victory) ui.edge(card, v2::kAccent);
        const Rect frame = v.victory ? ui.at(L::kPortrait125x191) : ui.at(L::kPortrait125x191.x, L::kPortrait170x258.y, 170, 258);
        ui.image(frame, ctx.ai_avatar(), draw::Fit::Contain, "A.I.", 1.0f);
        if (have1) ui.fill(Rect{frame.x0, frame.y1, frame.x1, frame.y1 + ui.px(6.0f)}, tc1);
        ui.publish_region("result/ai_portrait", frame);
        const Rect base = ui.at(L::kName2);
        const Rect nr{frame.x1 + ui.px(24.0f), base.y0, base.x1, base.y1};
        std::string name = "A.I.";
        if (v.tier > 0) name += std::string(" - ") + ShellSettings::difficulty_name(v.tier);
        ui.text(nr, name, v2::kH3, v.victory ? v2::kTextMuted : v2::kTextHi);
        ui.text(Rect{nr.x0, nr.y1 + ui.px(12.0f), nr.x1, nr.y1 + ui.px(40.0f)}, race_word(v.race[1]), v2::kBody, v2::kTextBody);
        if (!v.victory) ui.text(Rect{nr.x0, nr.y1 + ui.px(56.0f), nr.x1, nr.y1 + ui.px(80.0f)}, "WINNER", v2::kLabel, v2::kOk);
    }

    {
        const Rect t = ui.at(L::kScoreTable);
        ui.panel(t);
        ui.publish_region("result/score", t);
        const std::vector<ScoreRow>& rows = *v.score;
        const int head = ui.px(44.0f), rh = ui.px(40.0f), pad = ui.px(24.0f);
        const int bar_w = ui.px(400.0f), val_w = ui.px(220.0f);
        const int bx0 = (t.x0 + t.x1 - bar_w) / 2 + ui.px(120.0f);
        if (rows.empty()) {
            ui.text(t, "No score was recorded for this match.", v2::kBody, v2::kTextMuted, Align::Centre);
        } else {
            ui.text(Rect{bx0 - val_w, t.y0, bx0 - pad, t.y0 + head}, "YOU", v2::kLabel, v2::kTextMuted, Align::Right);
            ui.text(Rect{bx0 + bar_w + pad, t.y0, bx0 + bar_w + val_w, t.y0 + head}, "OPPONENT", v2::kLabel, v2::kTextMuted);
            ui.fill(Rect{t.x0, t.y0 + head - std::max(1, ui.px(2.0f)), t.x1, t.y0 + head}, v2::kLine1);
            const int shown = std::min(int(rows.size()), (t.h() - head) / std::max(1, rh));
            for (int i = 0; i < shown; ++i) {
                const ScoreRow& r = rows[std::size_t(i)];
                const int y0 = t.y0 + head + rh * i, y1 = y0 + rh;
                ui.text(Rect{t.x0 + pad, y0, bx0 - val_w, y1}, r.publish_name.empty() ? r.id : r.publish_name, v2::kBody, v2::kTextBody);
                ui.text(Rect{bx0 - val_w, y0, bx0 - pad, y1}, std::to_string(r.value[0]), v2::kBody, v2::kTextHi, Align::Right);
                ui.text(Rect{bx0 + bar_w + pad, y0, bx0 + bar_w + val_w, y1}, std::to_string(r.value[1]), v2::kBody, v2::kTextHi);
                const Rect bar{bx0, y0 + rh / 2 - ui.px(6.0f), bx0 + bar_w, y0 + rh / 2 + ui.px(6.0f)};
                ui.fill(bar, v2::kSurface3);
                const long long a = std::max(0, r.value[0]), b = std::max(0, r.value[1]);
                if (a + b > 0) {
                    const int split = bar.x0 + int(a * bar.w() / (a + b));
                    ui.fill(Rect{bar.x0, bar.y0, split, bar.y1}, have0 ? tc0 : v2::kTextBody);
                    ui.fill(Rect{split, bar.y0, bar.x1, bar.y1}, have1 ? tc1 : v2::kAccent);
                }
            }
        }
    }

    {
        Opts o;
        o.trigger = "PlayAgainButton";
        o.hint = "The same setup again.";
        if (ui.button("PlayAgainButton", ui.at(L::kPlayAgain), "Play again", ButtonKind::Primary, o) && !ctx.start_selected_match("PlayAgainButton"))
            ctx.fx.toast("The match could not be started.", true);
        Opts c;
        c.trigger = "ChangeSetupButton";
        c.hotkey = 'S';
        c.hint = "Back to the skirmish setup.";
        if (ui.button("ChangeSetupButton", ui.at(L::kChangeSetup), "Change setup", ButtonKind::Normal, c)) ctx.router.follow("ChangeSetupButton");
        std::error_code ec;
        if (!v.replay.empty() && std::filesystem::exists(v.replay, ec)) {
            Opts w;
            w.trigger = "WatchReplayButton";
            w.hotkey = 'W';
            w.hint = "Watch the recording of this match.";
            if (ui.button("WatchReplayButton", ui.at(L::kWatchReplay), "Watch replay", ButtonKind::Normal, w) && !ctx.watch_replay(v.replay, "WatchReplayButton"))
                ctx.fx.toast("That recording could not be read.", true);
        }
        Opts m;
        m.trigger = "Leave";
        m.hint = "Back to the main menu (Esc).";
        if (ui.button("MainMenuButton", ui.at(L::kMainMenu), "Main menu", ButtonKind::Normal, m)) ctx.router.follow("Leave");
    }
    toasts(ui, ctx);
    return true;
}

}
