#include "omc/app/shell_screens.hpp"

#include "omc/app/install.hpp"
#include "omc/app/shell_emblem.hpp"
#include "omc/core/client_identity.hpp"

namespace omc::app::shell {

namespace v2 = theme::v2;
namespace L = layout::s90_about;

bool screen_about(Ui& ui, ShellCtx& ctx) {
    chrome(ui, ctx, "ABOUT", kChromeBack | kChromePlate, "ESC Back");
    const Rect left = ui.at(L::kLicenceNotices);
    ui.panel(left, "Licences");
    const int pad = ui.px(24.0f), lh = ui.px(30.0f);
    int y = left.y0 + ui.px(72.0f);
    ui.text(Rect{left.x0 + pad, y, left.x1 - pad, y + lh}, std::string(identity::kName) + " is free software under the GNU General Public License, version 3 or later.",
            v2::kBody, v2::kTextHi);
    y += lh;
    ui.text(Rect{left.x0 + pad, y, left.x1 - pad, y + lh}, "It ships no game code or assets: everything shown from the game is read from your own installed copy.",
            v2::kSmall, v2::kTextBody);
    y += lh;
    ui.text(Rect{left.x0 + pad, y, left.x1 - pad, y + lh}, "It is an independent project, not affiliated with or endorsed by the game's publisher.", v2::kSmall,
            v2::kTextBody);
    y += lh + ui.px(16.0f);
    const std::string& notice = ctx.about_notice();
    int lines = 0;
    std::size_t start = 0;
    while (start < notice.size() && y + lh <= left.y1 - pad) {
        std::size_t nl = notice.find('\n', start);
        if (nl == std::string::npos) nl = notice.size();
        const std::string line = notice.substr(start, nl - start);
        const bool head = !line.empty() && line[0] != ' ';
        ui.text(Rect{left.x0 + pad, y, left.x1 - pad, y + lh}, line, head ? v2::kBody : v2::kSmall, head ? v2::kTextHi : v2::kTextBody);
        y += line.empty() ? lh / 2 : lh;
        ++lines;
        start = nl + 1;
    }
    ui.publish_region("about/notice", left, std::to_string(lines));

    const Rect right = ui.at(L::kEmblem256);
    ui.panel(right);
    const int cx = (right.x0 + right.x1) / 2, em = ui.px(256.0f);
    const Rect emr{cx - em / 2, right.y0 + ui.px(48.0f), cx + em / 2, right.y0 + ui.px(48.0f) + em};
    ui.picture(emr, &shell_emblem(false));
    ui.publish_region("about/emblem", emr);
    int ry = emr.y1 + ui.px(32.0f);
    const auto line = [&](const std::string& id, const std::string& text, theme::Type role, theme::Rgb c, float h_pt) {
        const Rect r{right.x0 + pad, ry, right.x1 - pad, ry + ui.px(h_pt)};
        ui.text(r, text, role, c, Align::Centre);
        ui.publish_region(id, r, text);
        ry = r.y1 + ui.px(8.0f);
    };
    line("about/name", identity::kName, v2::kH3, v2::kTextHi, 40.0f);
    line("about/version", ctx.version(), v2::kBody, v2::kTextBody, 32.0f);
    ry += ui.px(24.0f);
    line("about/build", std::string("Game build supported: ") + supported_build(), v2::kSmall, v2::kTextBody, 28.0f);
    const ShellCtx::GameData gd = ctx.game_data();
    line("about/game_data", gd.version.empty() ? std::string("No game folder reported") : "Game data: " + gd.version + (gd.verified ? " (verified)" : " (unverified build)"),
         v2::kSmall, !gd.valid ? v2::kTextMuted : (gd.verified ? v2::kOk : v2::kWarn), 28.0f);
    ry += ui.px(24.0f);
    line("about/config_label", "SETTINGS FILE", v2::kLabel, v2::kTextMuted, 24.0f);
    line("about/config", ctx.config_file().empty() ? std::string("kept in memory only") : ctx.config_file(), v2::kSmall, v2::kTextBody, 28.0f);
    toasts(ui, ctx);
    return true;
}

}
