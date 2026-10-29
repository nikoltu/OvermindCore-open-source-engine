#include "omc/app/shell_screens.hpp"

#include <algorithm>

#include "omc/app/shell_emblem.hpp"

namespace omc::app::shell {

namespace v2 = theme::v2;

constexpr std::uint8_t kOverMatchAlpha = 210;

ScreenFn screen_fn(ScreenId id) {
    switch (id) {
        case ScreenId::GlueRoot: return &screen_home;
        case ScreenId::Lobby: return &screen_setup;
        case ScreenId::Loading: return &screen_loading;
        case ScreenId::GameResult: return &screen_result;
        case ScreenId::Score: return &screen_score;
        case ScreenId::Options: return &screen_settings;
        case ScreenId::Profile: return &screen_profile;
        case ScreenId::Portraits: return &screen_portraits;
        case ScreenId::ReplayBrowse: return &screen_replays;
        case ScreenId::Credits: return &screen_about;
        case ScreenId::Widgets: return &screen_widgets;
        default: return nullptr;
    }
}

bool chrome(Ui& ui, ShellCtx& ctx, const std::string& title, unsigned flags, const std::string& legend) {
    namespace L = layout::s10_home;
    bool back = false;
    const std::vector<FluidSpan> body_spans = ui.fluid_spans();
    ui.fluid({FluidSpan{720.0f, 1700.0f, 1.0f}});
    ui.show_badges = ctx.settings.hotkey_badges();
    if (ctx.in_match()) ui.fill(Rect{0, 0, int(ui.view_w()), int(ui.view_h())}, v2::kBgBase, kOverMatchAlpha);
    else ui.background(ctx.settings.bg_motion());
    const int line = std::max(1, ui.px(v2::kLineW));
    if (!(flags & kChromeBare)) {
        const Rect bar = ui.band(L::kTopBar.y, L::kTopBar.h);
        ui.fill(bar, v2::kSurface1);
        ui.fill(Rect{bar.x0, bar.y1 - line, bar.x1, bar.y1}, v2::kLine1);
        ui.publish_region("chrome/top", bar);
        if (flags & kChromeHome) {
            const Rect em = ui.at(L::kEmblem);
            const IconAtlas::Icon& icon = shell_emblem(true);
            ui.picture(em, &icon);
            ui.text(ui.at(L::kProductWordmark), "OVERMINDCORE", v2::kH2, v2::kTextHi);
        } else {
            if (flags & kChromeBack) {
                const FlowEdge* e = ctx.router.cancel_edge();
                Opts o;
                o.trigger = e ? e->trigger : "";
                o.hint = "Back (Esc).";
                if (ui.button("chrome/back", ui.at(layout::s20_skirmish_setup::kBack), "<", ButtonKind::Ghost, o)) {
                    ctx.router.cancel();
                    back = true;
                }
            }
            ui.text(ui.at(layout::s20_skirmish_setup::kSkirmish), title, v2::kH2, v2::kTextHi);
        }
        if (flags & kChromePlate) {
            const Rect plate = ui.at(L::kIdentityPlate);
            const bool can_open = ctx.router.edge_for("ProfileButton") != nullptr;
            const std::string name = ctx.player_name();
            if (can_open) {
                Opts o;
                o.trigger = "ProfileButton";
                o.hint = "Your name, race, portrait and record.";
                if (ui.button("chrome/plate", plate, "", ButtonKind::Normal, o)) ctx.router.follow("ProfileButton");
            } else {
                ui.fill(plate, v2::kSurface2);
                ui.edge(plate, v2::kLine2);
                ui.publish_region("chrome/plate", plate, name);
            }
            const Rect frame{plate.x0, plate.y0, plate.x0 + ui.px(47.0f), plate.y1};
            ui.portrait(frame, ctx.player_portrait());
            ui.publish_region("chrome/plate_portrait", frame);
            const int tx = frame.x1 + ui.px(16.0f);
            ui.text(Rect{tx, plate.y0 + ui.px(8.0f), plate.x1 - ui.px(12.0f), plate.y0 + ui.px(40.0f)}, name, v2::kH3, v2::kTextHi);
            ui.text(Rect{tx, plate.y0 + ui.px(40.0f), plate.x1 - ui.px(12.0f), plate.y1 - ui.px(6.0f)}, lobby_race_name(ctx.settings.profile_race()), v2::kSmall,
                    v2::kTextMuted);
        }
    }
    const Rect hint = ui.band(L::kHintBar.y, L::kHintBar.h);
    ui.fill(hint, v2::kSurface1);
    ui.fill(Rect{hint.x0, hint.y0, hint.x1, hint.y0 + line}, v2::kLine1);
    ui.publish_region("chrome/hint", hint);
    const Rect block = ui.at(L::kHintBar);
    const int margin = ui.px(v2::kMargin);
    const std::string ver = std::string("overmindcore ") + ctx.version();
    const int vw = ui.text_width(ver, v2::kSmall);
    const Rect vr{block.x1 - margin - vw, hint.y0, block.x1 - margin, hint.y1};
    ui.text(vr, ver, v2::kSmall, v2::kTextMuted, Align::Right);
    ui.publish_region("chrome/version", vr, ver);
    const int lw = legend.empty() ? 0 : ui.text_width(legend, v2::kSmall);
    const int lx = (block.x0 + block.x1 - lw) / 2;
    if (lw) ui.text(Rect{lx, hint.y0, lx + lw, hint.y1}, legend, v2::kSmall, v2::kTextMuted);
    const int help_x1 = lw ? lx - margin / 2 : vr.x0 - margin / 2;
    ui.text(Rect{block.x0 + margin, hint.y0, help_x1, hint.y1}, ui.hint(), v2::kSmall, v2::kTextBody);
    ui.fluid(body_spans);
    return back;
}

void toasts(Ui& ui, ShellCtx& ctx) {
    if (!ui.painting()) return;
    const std::vector<ShellFx::Toast>& ts = ctx.fx.toasts();
    int y = ui.at(0, 112, 0, 0).y0;
    const int h = ui.px(64.0f), pad = ui.px(32.0f), gap = ui.px(8.0f);
    for (const ShellFx::Toast& t : ts) {
        const int tw = std::min(int(ui.view_w()) - pad * 2, ui.text_width(t.text, v2::kBody) + pad * 2);
        const int x0 = (int(ui.view_w()) - tw) / 2;
        const Rect r{x0, y, x0 + tw, y + h};
        const float fade = t.ttl_ms < 400.0f ? t.ttl_ms / 400.0f : 1.0f;
        ui.fill(r, v2::kSurface2, std::uint8_t(255.0f * fade));
        ui.edge(r, t.error ? v2::kErr : v2::kLine3, v2::kLineW, std::uint8_t(255.0f * fade));
        if (fade > 0.5f) ui.text(r.inset(ui.px(12.0f)), t.text, v2::kBody, t.error ? v2::kErr : v2::kTextBody, Align::Centre);
        y += h + gap;
    }
}

Confirm confirm(Ui& ui, const std::string& id, const std::string& title, const std::string& body, const std::string& no_label,
                const std::string& yes_label, bool danger) {
    namespace M = layout::m01_confirm;
    const bool was_open = ui.state().modal == id;
    const std::vector<FluidSpan> body_spans = ui.fluid_spans();
    ui.fluid_centred(M::kDialog640x320.x, M::kDialog640x320.x + M::kDialog640x320.w);
    if (!ui.modal_begin(id, ui.at(M::kDialog640x320), title)) {
        ui.fluid(body_spans);
        return was_open ? Confirm::No : Confirm::None;
    }
    Confirm out = Confirm::None;
    const Rect br = ui.at(M::kBody);
    const int lh = br.h() / 3;
    std::size_t start = 0;
    for (int line = 0; line < 3 && start <= body.size(); ++line) {
        std::size_t nl = body.find('\n', start);
        if (nl == std::string::npos) nl = body.size();
        ui.text(Rect{br.x0, br.y0 + lh * line, br.x1, br.y0 + lh * (line + 1)}, body.substr(start, nl - start), v2::kBody, v2::kTextBody, Align::Centre);
        start = nl + 1;
    }
    Opts no;
    no.hint = "Esc";
    if (ui.button(id + "/no", ui.at(M::kCancel), no_label, ButtonKind::Normal, no)) out = Confirm::No;
    Opts yes;
    yes.danger = danger;
    yes.hint = "Enter";
    if (ui.button(id + "/yes", ui.at(M::kConfirm), yes_label, danger ? ButtonKind::Normal : ButtonKind::Primary, yes)) out = Confirm::Yes;
    ui.modal_end();
    ui.fluid(body_spans);
    if (out != Confirm::None) ui.close_modal();
    return out;
}

}
