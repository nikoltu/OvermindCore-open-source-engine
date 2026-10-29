#include "omc/app/melee_game.hpp"

#include <algorithm>
#include <cmath>

#include "omc/app/ui_hotkeys.hpp"
#include "omc/data/catalog.hpp"

namespace omc::app {

void MeleeGame::steal_control_groups(int keep) {
    for (int o = 0; o < 10; ++o) {
        if (o == keep) continue;
        std::vector<std::uint32_t> rest;
        for (std::uint32_t id : groups_.recall(o))
            if (std::find(selection_.begin(), selection_.end(), id) == selection_.end()) rest.push_back(id);
        set_control_group(o, rest);
    }
}

bool MeleeGame::hud_selection_on_button(const HudPress& p) {
    const int i = int(p.id);
    if (i < int(HudButtonId::Group0) || i > int(HudButtonId::Group9)) return false;
    const int slot = i - int(HudButtonId::Group0);
    if (p.left) {
        const std::vector<std::uint32_t> g = groups_.recall(slot);
        if (g.empty()) return false;
        set_selection(g);
        recall_double_tap(slot);
        return true;
    }
    if (p.right) {
        if (p.alt) steal_control_groups(slot);
        set_control_group(slot, selection_);
        return true;
    }
    return false;
}

bool MeleeGame::alert_recall() {
    std::vector<const HudMessage*> targets;
    for (const HudMessage& m : messages_.all())
        if (m.zone == MsgZone::Alert && m.has_target) targets.push_back(&m);
    HudSelectionUi& s = hud_ui_.selection;
    if (targets.empty()) {
        s.alert_cycle = 0;
        return false;
    }
    if (int(targets.size()) != s.alert_count) {
        s.alert_count = int(targets.size());
        s.alert_cycle = 0;
    }
    const HudMessage& m = *targets[targets.size() - 1 - std::size_t(s.alert_cycle) % targets.size()];
    s.alert_cycle = (s.alert_cycle + 1) % int(targets.size());
    camctl_.focus = {m.tx, m.ty, ground_z_at(m.tx, m.ty)};
    user_scrolled_ = true;
    return true;
}

float hud_helper_glow(float t) {
    const float phase = std::fmod(t, 3.0f) / 1.5f;
    const float x = phase <= 1.0f ? phase : 2.0f - phase;
    return x * x * (3.0f - 2.0f * x);
}

}
