#include "omc/app/shell_screens.hpp"

#include <algorithm>

namespace omc::app::shell {

namespace v2 = theme::v2;
namespace L = layout::s20_skirmish_setup;

namespace {

constexpr layout::R kStartsNote{896, 128, 581, 24};
constexpr layout::R kOptionsLabel{736, 728, 741, 24};
constexpr layout::R kSpeedLabel{736, 764, 300, 24};
constexpr layout::R kSpeed{736, 796, 741, 64};
constexpr layout::R kTeamsLabel{736, 880, 300, 24};
constexpr layout::R kTeams{736, 912, 741, 64};
constexpr layout::R kBanner{736, 1000, 741, 72};

int seated(const ShellCtx& ctx) {
    int n = 0;
    for (int i = 0; i < ctx.lobby_rows(); ++i) {
        int ctl = 2, race = 0, diff = 0, team = -1;
        ctx.lobby_row(i, ctl, race, diff, team);
        n += ctl == 0 || ctl == 1;
    }
    return n;
}

int teams_mode(const ShellCtx& ctx) {
    for (int i = 0; i < ctx.lobby_rows(); ++i) {
        int ctl = 2, race = 0, diff = 0, team = -1;
        ctx.lobby_row(i, ctl, race, diff, team);
        if ((ctl == 0 || ctl == 1) && team >= 0) return 1;
    }
    return 0;
}

void set_teams_mode(ShellCtx& ctx, int mode) {
    const int players = seated(ctx);
    int k = 0;
    for (int i = 0; i < ctx.lobby_rows(); ++i) {
        int ctl = 2, race = 0, diff = 0, team = -1;
        ctx.lobby_row(i, ctl, race, diff, team);
        if (ctl != 0 && ctl != 1) continue;
        const int want = mode == 0 ? -1 : (k < (players + 1) / 2 ? 0 : 1);
        ++k;
        ctx.set_lobby_row(i, ctl, race, diff, want);
    }
}

}

bool screen_custom(Ui& ui, ShellCtx& ctx) {
    chrome(ui, ctx, "CUSTOM GAME", kChromeBack | kChromePlate, "ENTER Start   ESC Back");
    ui.fluid({FluidSpan{L::kMapListViewport.x, L::kMapListViewport.x + L::kMapListViewport.w, 48.0f / 213.0f},
              FluidSpan{L::kRaceTerranZergProtoss.x, L::kRaceTerranZergProtoss.x + L::kRaceTerranZergProtoss.w, 165.0f / 213.0f}});
    map_picker(ui, ctx, "custom");
    ctx.prepare_custom_lobby();
    const std::vector<platform::MapEntry>& maps = ctx.maps();
    const int selected = ctx.selected_map();
    const bool have_sel = selected >= 0 && selected < int(maps.size());
    const bool sel_arena = have_sel && maps[std::size_t(selected)].path.empty();
    const int starts = have_sel ? ctx.map_start_count(selected) : 0;

    ui.text(ui.at(L::kLabelPlayers), "PLAYERS", v2::kLabel, v2::kTextMuted);
    if (have_sel)
        ui.text(ui.at(kStartsNote), std::to_string(starts) + " start locations - one row each", v2::kSmall, v2::kTextFaint, Align::Right);
    slot_rows(ui, ctx, true);

    ui.text(ui.at(kOptionsLabel), "MAP OPTIONS", v2::kLabel, v2::kTextMuted);
    ui.text(ui.at(kSpeedLabel), "GAME SPEED", v2::kSmall, v2::kTextBody);
    {
        int sp = int(ctx.settings.game_speed());
        Opts o;
        o.hint = "How fast the match runs. Ladder melee runs at Faster.";
        if (ui.segmented("custom/speed", ui.at(kSpeed), {"Slower", "Slow", "Normal", "Fast", "Faster"}, sp, o) &&
            !ctx.settings.set(ShellSettings::kGameSpeedKey, std::to_string(sp)))
            ctx.fx.toast("Setting could not be saved", true);
    }
    ui.text(ui.at(kTeamsLabel), "TEAMS", v2::kSmall, v2::kTextBody);
    {
        int mode = teams_mode(ctx);
        Opts o;
        o.enabled = seated(ctx) >= 2;
        o.hint = "Free for all: everyone alone. Two teams: the seated rows split into two allied halves; each row's TEAM can still be changed.";
        if (ui.segmented("custom/teams", ui.at(kTeams), {"Free for all", "Two teams"}, mode, o)) set_teams_mode(ctx, mode);
    }
    const std::string& refusal = ctx.match_error();
    if (!refusal.empty()) {
        ui.banner(ui.at(kBanner), State::Err, "Refused", refusal);
        ui.publish_region("custom/banner", ui.at(kBanner), refusal);
    }

    ui.text(ui.at(L::kLabelMatch), "MATCH", v2::kLabel, v2::kTextMuted);
    const std::string sel_name = !have_sel ? std::string() : (sel_arena ? std::string("TEST ARENA (built-in)") : maps[std::size_t(selected)].name);
    ui.image(ui.at(L::kMapPreviewWell), have_sel ? ctx.map_preview(selected) : nullptr, draw::Fit::Contain,
             !have_sel ? "No map selected" : (sel_arena ? "The built-in arena has no picture" : "No preview in this map"), 2.0f);
    ui.text(ui.at(L::kMapName), sel_name, v2::kH3, v2::kTextHi);
    if (have_sel)
        ui.text(ui.at(L::kSize),
                std::to_string(starts) + " players - " +
                    (sel_arena ? std::string("built-in") : (maps[std::size_t(selected)].casc ? std::string("INSTALL") : maps[std::size_t(selected)].path)),
                v2::kSmall, v2::kTextMuted);
    {
        const Rect card = ui.at(L::kSummaryMatchup);
        ui.panel(card);
        const int pad = ui.px(24.0f);
        ui.text(Rect{card.x0 + pad, card.y0 + ui.px(16.0f), card.x1 - pad, card.y0 + ui.px(48.0f)}, team_matchup(ctx), v2::kBody, v2::kTextHi);
        ui.text(Rect{card.x0 + pad, card.y0 + ui.px(50.0f), card.x1 - pad, card.y0 + ui.px(80.0f)},
                std::string("CUSTOM MELEE - ") + ShellSettings::speed_name(ctx.settings.game_speed()), v2::kSmall, v2::kTextMuted);
    }
    {
        const int players = seated(ctx);
        const bool can = have_sel && players >= 2;
        const std::string why = !have_sel ? std::string("Pick a map first.") : "Seat two players or more (You or A.I.).";
        Opts o;
        o.trigger = "CustomStartButton";
        o.enabled = can;
        o.hint = "Load the match. Open rows stay empty offline.";
        o.why_disabled = why;
        if (!can) ui.text(ui.at(L::kWhyStartIsDisabled), why, v2::kSmall, v2::kWarn);
        if (ui.button("custom/start", ui.at(L::kStartGame), "Start game", ButtonKind::Primary, o) && !ctx.start_selected_match("CustomStartButton"))
            ctx.fx.toast("The match could not be started.", true);
    }
    toasts(ui, ctx);
    return true;
}

}
