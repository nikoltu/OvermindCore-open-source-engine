#include "omc/app/shell_screens.hpp"

namespace omc::app::shell {

namespace v2 = theme::v2;

namespace {

constexpr layout::R kCard{64, 160, 1349, 640};
constexpr layout::R kHeadline{112, 240, 1253, 48};
constexpr layout::R kLine1{112, 304, 1253, 32};
constexpr layout::R kLine2{112, 340, 1253, 32};
constexpr layout::R kListLabel{112, 412, 1253, 24};
constexpr float kListTop = 448.0f, kListPitch = 56.0f;
constexpr layout::R kSide{1445, 160, 624, 640};

}

bool screen_campaign(Ui& ui, ShellCtx& ctx) {
    chrome(ui, ctx, "CAMPAIGN", kChromeBack | kChromePlate, "ESC Back");
    ui.panel(ui.at(kCard), "Coming soon");
    ui.text(ui.at(kHeadline), "The story campaigns come after version 1.0", v2::kH2, v2::kTextHi);
    ui.text(ui.at(kLine1), "This client plays melee today: skirmish and custom games against the A.I.", v2::kBody, v2::kTextBody);
    ui.text(ui.at(kLine2), "Your progress will be kept here once the campaigns open.", v2::kBody, v2::kTextBody);
    const std::vector<std::string>& titles = ctx.campaign_titles();
    ui.text(ui.at(kListLabel), titles.empty() ? "NO CAMPAIGN FOUND IN THE GAME DATA" : "IN YOUR GAME DATA", v2::kLabel, v2::kTextMuted);
    for (std::size_t i = 0; i < titles.size() && i < 5; ++i) {
        const Rect r = ui.at(112.0f, kListTop + kListPitch * float(i), 1253.0f, 48.0f);
        ui.fill(Rect{r.x0, r.y0 + ui.px(12.0f), r.x0 + ui.px(6.0f), r.y1 - ui.px(12.0f)}, v2::kLine3);
        ui.text(Rect{r.x0 + ui.px(24.0f), r.y0, r.x1 - ui.px(200.0f), r.y1}, titles[i], v2::kH3, v2::kTextHi);
        ui.text(Rect{r.x1 - ui.px(200.0f), r.y0, r.x1, r.y1}, "SOON", v2::kLabel, v2::kTextMuted, Align::Right);
        ui.publish_region("campaign/title" + std::to_string(i), r, titles[i]);
    }
    const Rect side = ui.at(kSide);
    ui.panel(side, "Meanwhile");
    ui.text(Rect{side.x0 + ui.px(24.0f), side.y0 + ui.px(80.0f), side.x1 - ui.px(24.0f), side.y0 + ui.px(112.0f)}, "Every melee map in your game folder is playable.",
            v2::kBody, v2::kTextBody);
    ui.text(Rect{side.x0 + ui.px(24.0f), side.y0 + ui.px(116.0f), side.x1 - ui.px(24.0f), side.y0 + ui.px(148.0f)}, "Back (Esc) leads to Home.", v2::kSmall,
            v2::kTextMuted);
    toasts(ui, ctx);
    return true;
}

}
