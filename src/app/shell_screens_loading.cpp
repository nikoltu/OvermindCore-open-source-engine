#include "sc2/app/shell_screens.hpp"

#include <cstdio>

namespace sc2::app::shell {

namespace v2 = theme::v2;
namespace L = layout::s30_loading;

namespace {
const char* race_word(Race r) { return r == Race::Protoss ? "Protoss" : (r == Race::Zerg ? "Zerg" : "Terran"); }
LobbyRace as_lobby(Race r) { return r == Race::Protoss ? LobbyRace::Protoss : (r == Race::Zerg ? LobbyRace::Zerg : LobbyRace::Terran); }
}

bool screen_loading(Ui& ui, ShellCtx& ctx) {
    chrome(ui, ctx, "", kChromeBare, "ESC Cancel");
    ui.fluid_centred(L::kH1MapName.x, L::kH1MapName.x + L::kH1MapName.w);
    const ShellCtx::LoadView v = ctx.loading();

    ui.text(ui.at(L::kH1MapName), v.map, v2::kH1, v2::kTextHi, Align::Centre);
    const std::string mode = v.replay ? std::string("REPLAY") : std::string("1V1 MELEE - ") + ShellSettings::speed_name(ctx.settings.game_speed());
    ui.text(ui.at(L::kLabel1v1Melee), mode, v2::kLabel, v2::kTextMuted, Align::Centre);

    {
        ui.panel(ui.at(L::kPlayerCard));
        theme::Rgb tc{};
        const bool have_tc = ctx.team_color(0, tc);
        const Rect frame = ui.at(L::kPortraitFrame170x258);
        ui.portrait(frame, ctx.player_portrait());
        ui.publish_region("loading/you_portrait", frame);
        ui.text(ui.at(L::kName), ctx.player_name(), v2::kH3, v2::kTextHi);
        const Rect mark = ui.at(L::kRaceMark);
        const IconAtlas::Icon* icon = ctx.race_mark(as_lobby(v.race[0]));
        ui.picture(mark, icon);
        const Rect rr = ui.at(L::kRace);
        ui.text(icon ? rr : Rect{mark.x0, rr.y0, rr.x1, rr.y1}, race_word(v.race[0]), v2::kBody, v2::kTextBody);
        if (have_tc) ui.fill(ui.at(L::kTeamColourBar), tc);
    }
    ui.text(ui.at(L::kVs), "VS", v2::kH1, v2::kTextFaint, Align::Centre);
    {
        const Rect card = ui.at(L::kPlayerCard2);
        ui.panel(card);
        theme::Rgb tc{};
        const bool have_tc = ctx.team_color(1, tc);
        const Rect frame = ui.at(L::kAIAvatarFrame);
        ui.image(frame, ctx.ai_avatar(), draw::Fit::Contain, "A.I.", 1.0f);
        ui.publish_region("loading/ai_portrait", frame);
        std::string name = "A.I.";
        if (!v.replay) name += std::string(" - ") + ShellSettings::difficulty_name(ctx.settings.ai_difficulty());
        ui.text(ui.at(L::kAI), name, v2::kH3, v2::kTextHi, Align::Right);
        const Rect nr = ui.at(L::kAI);
        const int mark_side = ui.px(48.0f);
        const IconAtlas::Icon* icon = ctx.race_mark(as_lobby(v.race[1]));
        const Rect mark{nr.x1 - mark_side, nr.y1 + ui.px(16.0f), nr.x1, nr.y1 + ui.px(16.0f) + mark_side};
        ui.picture(mark, icon);
        ui.text(Rect{nr.x0, mark.y0 + ui.px(8.0f), icon ? mark.x0 - ui.px(12.0f) : nr.x1, mark.y0 + ui.px(36.0f)}, race_word(v.race[1]), v2::kBody, v2::kTextBody,
                Align::Right);
        if (have_tc) ui.fill(Rect{nr.x1 - ui.px(360.0f), mark.y1 + ui.px(24.0f), nr.x1, mark.y1 + ui.px(32.0f)}, tc);
    }

    ui.image(ui.at(L::kMapPreview240), v.map_index > 0 ? ctx.map_preview(v.map_index) : nullptr, draw::Fit::Contain, "No preview", 2.0f);
    ui.text(ui.at(L::kTipLine), "Hold Alt to show the life bars of every unit.", v2::kSmall, v2::kTextMuted, Align::Centre);

    const Rect bar = ui.at(L::kProgressBarDoneTotal);
    ui.progress(bar, v.total ? float(v.done) / float(v.total) : -1.0f);
    ui.publish_region("loading/bar", bar);
    ui.text(ui.at(L::kStageLabel), v.total ? v.stage : std::string("Preparing"), v2::kSmall, v2::kTextBody);
    char buf[64];
    const int secs = int(v.elapsed_ms / 1000.0f);
    if (v.total) std::snprintf(buf, sizeof buf, "%d / %d   %d:%02d", int(v.done), int(v.total), secs / 60, secs % 60);
    else std::snprintf(buf, sizeof buf, "%d:%02d", secs / 60, secs % 60);
    ui.text(ui.at(L::kNTotal), buf, v2::kSmall, v2::kTextMuted, Align::Right);
    toasts(ui, ctx);
    return true;
}

}
