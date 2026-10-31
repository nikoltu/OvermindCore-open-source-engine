#include "omc/app/shell_screens.hpp"

namespace omc::app::shell {

namespace v2 = theme::v2;

namespace {

constexpr layout::R kNotice{64, 128, 2005, 72};
constexpr layout::R kGames{64, 232, 1349, 880};
constexpr float kColGame = 88.0f, kColMap = 600.0f, kColPlayers = 1100.0f, kHeadY = 296.0f;
constexpr layout::R kEmptyTitle{64, 560, 1349, 40};
constexpr layout::R kEmptyLine{64, 612, 1349, 32};
constexpr layout::R kRefresh{88, 1016, 300, 64};
constexpr layout::R kHost{1445, 232, 624, 880};
constexpr layout::R kHostGame{1469, 864, 576, 96};
constexpr layout::R kJoin{1469, 984, 576, 64};

}

bool screen_lan(Ui& ui, ShellCtx& ctx) {
    chrome(ui, ctx, "LAN GAME", kChromeBack | kChromePlate, "ESC Back");
    const std::string soon = "LAN play arrives in version 0.4.";
    ui.banner(ui.at(kNotice), State::Warn, "Not available yet", "This window shows what LAN play will offer. Nothing is sent on the network.");
    ui.publish_region("lan/notice", ui.at(kNotice), "Not available yet");

    const Rect games = ui.at(kGames);
    ui.panel(games, "Games on this network");
    ui.text(ui.at(kColGame, kHeadY, kColMap - kColGame - 16.0f, 24.0f), "GAME", v2::kLabel, v2::kTextMuted);
    ui.text(ui.at(kColMap, kHeadY, kColPlayers - kColMap - 16.0f, 24.0f), "MAP", v2::kLabel, v2::kTextMuted);
    ui.text(ui.at(kColPlayers, kHeadY, 280.0f, 24.0f), "PLAYERS", v2::kLabel, v2::kTextMuted);
    ui.fill(ui.at(kColGame, kHeadY + 32.0f, 1301.0f, 2.0f), v2::kLine1);
    ui.text(ui.at(kEmptyTitle), "No search running", v2::kH3, v2::kTextHi, Align::Centre);
    ui.text(ui.at(kEmptyLine), "Looking for games on this network starts with the network layer.", v2::kBody, v2::kTextBody, Align::Centre);
    {
        Opts o;
        o.enabled = false;
        o.why_disabled = soon;
        ui.button("lan/refresh", ui.at(kRefresh), "Refresh", ButtonKind::Normal, o);
    }

    const Rect host = ui.at(kHost);
    ui.panel(host, "Host a game");
    const int pad = ui.px(24.0f);
    ui.text(Rect{host.x0 + pad, host.y0 + ui.px(80.0f), host.x1 - pad, host.y0 + ui.px(104.0f)}, "HOST", v2::kLabel, v2::kTextMuted);
    ui.text(Rect{host.x0 + pad, host.y0 + ui.px(108.0f), host.x1 - pad, host.y0 + ui.px(144.0f)}, ctx.player_name(), v2::kH3, v2::kTextHi);
    const int map_index = ctx.setup_map();
    ui.text(Rect{host.x0 + pad, host.y0 + ui.px(176.0f), host.x1 - pad, host.y0 + ui.px(200.0f)}, "MAP", v2::kLabel, v2::kTextMuted);
    ui.text(Rect{host.x0 + pad, host.y0 + ui.px(204.0f), host.x1 - pad, host.y0 + ui.px(240.0f)}, map_index >= 0 ? ctx.map_title(map_index) : std::string("No maps found"),
            v2::kBody, v2::kTextBody);
    ui.text(Rect{host.x0 + pad, host.y0 + ui.px(280.0f), host.x1 - pad, host.y0 + ui.px(312.0f)}, "Players on this network will join your", v2::kSmall,
            v2::kTextMuted);
    ui.text(Rect{host.x0 + pad, host.y0 + ui.px(312.0f), host.x1 - pad, host.y0 + ui.px(344.0f)}, "game's open slots from their own client.", v2::kSmall, v2::kTextMuted);
    {
        Opts o;
        o.enabled = false;
        o.why_disabled = soon;
        ui.button("lan/host", ui.at(kHostGame), "Host game", ButtonKind::Primary, o);
        ui.button("lan/join", ui.at(kJoin), "Join by address", ButtonKind::Normal, o);
    }
    toasts(ui, ctx);
    return true;
}

}
