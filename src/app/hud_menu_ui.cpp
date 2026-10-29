#include "omc/app/melee_game.hpp"

#include "omc/app/overlay_draw.hpp"
#include "omc/app/ui_hotkeys.hpp"
#include "omc/data/catalog.hpp"

namespace omc::app {

bool MeleeGame::hud_menu_on_button(const HudPress& p) {
    (void)p;
    return false;
}

void MeleeGame::hud_menu_hotkeys(const render::InputState& in) {
    if (!cat_ || pause_menu_open_) return;
    if (!ui_hotkey_fires(cat_->hotkeys(), "PauseGame", in)) return;
    hud_ui_.menu.paused_by_key = !hud_ui_.menu.paused_by_key;
    clock_.set_paused(hud_ui_.menu.paused_by_key);
}

void MeleeGame::hud_menu_draw(std::uint32_t view_w, std::uint32_t view_h, std::vector<std::uint8_t>& out) {
    if (!hud_ui_.menu.paused_by_key || pause_menu_open_ || !cat_ || !ui_store_) return;
    const std::string path = cat_->resolve_asset("@UI/PauseScreenDimImage");
    if (path.empty() || path[0] == '@') return;
    const IconAtlas::Icon& mask = icons_.get(*ui_store_, path.c_str());
    if (!mask.ok()) return;
    for (std::uint32_t y = 0; y < view_h; ++y) {
        const std::size_t sy = std::size_t(y) * mask.h / view_h;
        for (std::uint32_t x = 0; x < view_w; ++x) {
            const std::size_t si = (sy * mask.w + std::size_t(x) * mask.w / view_w) * 4;
            const int a = int(mask.rgba[si + 3]) * 165 / 255;
            if (a > 0) draw::blend(out, view_w, view_h, int(x), int(y), mask.rgba[si], mask.rgba[si + 1], mask.rgba[si + 2], std::uint8_t(a));
        }
    }
}

}
