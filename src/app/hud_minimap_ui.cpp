#include "omc/app/melee_game.hpp"

#include <cstdio>
#include <cstdlib>

#include "omc/data/catalog.hpp"

namespace omc::app {

bool MeleeGame::hud_minimap_on_button(const HudPress& p) {
    if (!p.left) return false;
    if (p.id == HudButtonId::MinimapTerrain) {
        hud_ui_.minimap.show_terrain = !hud_ui_.minimap.show_terrain;
        return true;
    }
    if (p.id == HudButtonId::MinimapPing) {
        hud_ui_.minimap.ping_armed = !hud_ui_.minimap.ping_armed;
        if (hud_ui_.minimap.ping_armed) pending_attack_ = pending_patrol_ = pending_move_ = false;
        return true;
    }
    return false;
}

static bool mouse_spec_fires(const data::StringTable& hk, const char* name, const char* button, const render::InputState& in) {
    const std::string* spec = hk.find(std::string("UI/Hotkey/") + name);
    if (!spec) return false;
    std::size_t start = 0;
    while (start <= spec->size()) {
        std::size_t comma = spec->find(',', start);
        if (comma == std::string::npos) comma = spec->size();
        const std::string alt = spec->substr(start, comma - start);
        start = comma + 1;
        const std::size_t at = alt.rfind('+');
        if ((at == std::string::npos ? alt : alt.substr(at + 1)) != button) continue;
        const std::string mods = at == std::string::npos ? std::string() : alt.substr(0, at + 1);
        if ((mods.find("Control+") != std::string::npos) == in.key_ctrl && (mods.find("Shift+") != std::string::npos) == in.shift &&
            (mods.find("Alt+") != std::string::npos) == in.key_alt)
            return true;
    }
    return false;
}

bool MeleeGame::hud_minimap_take_click(const render::InputState& in, bool have_point, float wx, float wy) {
    static const bool legacy = std::getenv("OMC_HUD_BUTTONS_LEGACY") != nullptr;
    if (legacy || !cat_) return false;
    HudMinimapUi& m = hud_ui_.minimap;
    const data::StringTable& hk = cat_->hotkeys();
    if (m.ping_armed && in.right_pressed && mouse_spec_fires(hk, "TargetCancel", "RightMouseButton", in)) {
        m.ping_armed = false;
        return true;
    }
    if (!in.left_pressed) return false;
    const bool quick = !m.ping_armed && mouse_spec_fires(hk, "QuickPing", "LeftMouseButton", in);
    if (!m.ping_armed && !quick) return false;
    if (!have_point) return m.ping_armed;
    HudMinimapUi::Ping ping;
    ping.x = wx;
    ping.y = wy;
    m.pings.push_back(ping);
    m.ping_armed = false;
    if (std::getenv("OMC_ENV_TRACE")) std::fprintf(stderr, "[hud] ping at %.1f,%.1f%s\n", double(wx), double(wy), quick ? " (quick)" : "");
    return true;
}

void MeleeGame::hud_minimap_apply(HudContent& content, MinimapBackdrop& backdrop) {
    const HudMinimapUi& m = hud_ui_.minimap;
    if (!m.show_terrain) backdrop.terrain = nullptr;
    float ping_time = 6.0f;
    if (cat_)
        if (const auto pt = cat_->scalar("CAlert", "Attack", "PingTime"); pt && !pt->empty()) ping_time = float(std::atof(pt->c_str()));
    for (const HudMinimapUi::Ping& p : m.pings) {
        if (p.age_s >= ping_time) continue;
        HudContent::MinimapPing ping;
        ping.x = p.x;
        ping.y = p.y;
        ping.age = p.age_s;
        ping.ping_time = ping_time;
        ping.r = ping.g = ping.b = 255;
        content.pings.push_back(ping);
    }
}

}
