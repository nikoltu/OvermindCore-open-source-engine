#include "sc2/app/shell_screens.hpp"

#include <algorithm>
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
    std::vector<int> teams;
    for (const MeleeSlot& s : v.players) teams.push_back(s.team);
    const std::vector<std::vector<int>> cols = team_columns(teams);
    const std::string fmt = v.players.size() > 2 ? format_label(cols) : std::string("1V1");
    const std::string mode = v.replay ? std::string("REPLAY") : fmt + " MELEE - " + ShellSettings::speed_name(ctx.settings.game_speed());
    ui.text(ui.at(L::kLabel1v1Melee), mode, v2::kLabel, v2::kTextMuted, Align::Centre);

    if (v.players.size() > 2) {
        const float x0 = L::kPlayerCard.x, x1 = L::kPlayerCard2.x + L::kPlayerCard2.w;
        const float y0 = L::kPlayerCard.y, h = L::kPlayerCard.h, gap = 48.0f;
        std::size_t deepest = 1;
        for (const auto& c : cols) deepest = std::max(deepest, c.size());
        const float col_w = (x1 - x0 - gap * float(cols.size() - 1)) / float(cols.size());
        const float card_h = (h - 12.0f * float(deepest - 1)) / float(deepest);
        int you = -1;
        for (std::size_t p = 0; p < v.players.size(); ++p)
            if (v.players[p].controller == MeleeSlot::Controller::Human && you < 0) you = int(p);
        for (std::size_t c = 0; c < cols.size(); ++c) {
            const float cx = x0 + (col_w + gap) * float(c);
            if (c > 0) ui.text(ui.at(cx - gap, y0 + h / 2.0f - 20.0f, gap, 40.0f), "VS", v2::kLabel, v2::kTextFaint, Align::Centre);
            for (std::size_t k = 0; k < cols[c].size(); ++k) {
                const int p = cols[c][k];
                const MeleeSlot& s = v.players[std::size_t(p)];
                const Rect card = ui.at(cx, y0 + (card_h + 12.0f) * float(k), col_w, card_h);
                ui.panel(card);
                ui.publish_region("loading/player" + std::to_string(p), card);
                theme::Rgb tc{};
                if (ctx.team_color(p, tc)) ui.fill(Rect{card.x0, card.y0, card.x0 + ui.px(8.0f), card.y1}, tc);
                std::string name = p == you ? ctx.player_name() : std::string("A.I.");
                if (p != you && !v.replay)
                    name += std::string(" - ") + ShellSettings::difficulty_name(s.difficulty > 0 ? s.difficulty : ctx.settings.ai_difficulty());
                const int pad = ui.px(24.0f), mark = std::min(ui.px(48.0f), std::max(1, card.h() / 3));
                ui.text(Rect{card.x0 + pad, card.y0 + ui.px(12.0f), card.x1 - ui.px(12.0f), card.y0 + ui.px(48.0f)}, name, v2::kH3, v2::kTextHi);
                const IconAtlas::Icon* icon = ctx.race_mark(as_lobby(s.race));
                const Rect mr{card.x0 + pad, card.y0 + ui.px(56.0f), card.x0 + pad + mark, card.y0 + ui.px(56.0f) + mark};
                ui.picture(mr, icon);
                ui.text(Rect{icon ? mr.x1 + ui.px(12.0f) : mr.x0, mr.y0, card.x1 - ui.px(12.0f), mr.y1}, race_word(s.race), v2::kBody, v2::kTextBody);
            }
        }
    }

    if (v.players.size() <= 2) {
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
    if (v.players.size() <= 2) ui.text(ui.at(L::kVs), "VS", v2::kH1, v2::kTextFaint, Align::Centre);
    if (v.players.size() <= 2) {
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
