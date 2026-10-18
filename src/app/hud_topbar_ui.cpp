#include "sc2/app/melee_game.hpp"

namespace sc2::app {

bool MeleeGame::hud_topbar_on_button(const HudPress& p) {
    if (p.id != HudButtonId::Alliance || !p.left) return false;
    hud_ui_.topbar.alliance_open = !hud_ui_.topbar.alliance_open;
    return true;
}

}
