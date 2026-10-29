#include "omc/app/hud_buttons.hpp"

#include <cstdlib>

#include "omc/app/melee_game.hpp"
#include "omc/app/ui_hotkeys.hpp"
#include "omc/data/catalog.hpp"

namespace omc::app {

const char* hud_button_name(HudButtonId id) {
    static const char* const kNames[] = {
        "None", "MenuHelp", "MenuChat", "MenuSocial", "MenuMenu", "MinimapPing", "MinimapTerrain", "MinimapColor",
        "Alliance", "Idle", "Army", "Warp", "Group0", "Group1", "Group2", "Group3", "Group4", "Group5", "Group6",
        "Group7", "Group8", "Group9", "Clock", "Resource0", "Resource1", "Supply", "Wireframe", "Portrait", "Progress"};
    const std::size_t i = std::size_t(id);
    return i < sizeof kNames / sizeof kNames[0] ? kNames[i] : "?";
}

const char* hud_look_suffix(HudLook look) {
    switch (look) {
        case HudLook::Hover: return "hover";
        case HudLook::Pressed: return "pressed";
        case HudLook::Selected: return "selected";
        case HudLook::Disabled: return "disabled";
        default: return "normal";
    }
}

void HudButtons::add(const HudButton& b) {
    if (b.rect.w() <= 0 || b.rect.h() <= 0) return;
    HudButton c = b;
    if (c.hit.w() <= 0 || c.hit.h() <= 0) c.hit = c.rect;
    buttons_.push_back(std::move(c));
}

const HudButton* HudButtons::find(HudButtonId id) const {
    for (const HudButton& b : buttons_)
        if (b.id == id) return &b;
    return nullptr;
}

static bool holds(const HudRect& r, float x, float y) {
    return x >= float(r.x0) && x < float(r.x1) && y >= float(r.y0) && y < float(r.y1);
}

const HudButton* HudButtons::at(float x, float y) const {
    for (std::size_t i = buttons_.size(); i-- > 0;)
        if (holds(buttons_[i].hit, x, y)) return &buttons_[i];
    return nullptr;
}

std::optional<HudPress> HudButtons::route(const render::InputState& in) {
    if (!in.left_down && !in.left_pressed) pressed_ = HudButtonId::None;
    if (!in.left_pressed && !in.right_pressed) return std::nullopt;
    for (std::size_t i = buttons_.size(); i-- > 0;) {
        const HudButton& b = buttons_[i];
        if (b.hover_only || !b.enabled || !holds(b.hit, in.mouse_x, in.mouse_y)) continue;
        HudPress p;
        p.id = b.id;
        p.left = in.left_pressed;
        p.right = in.right_pressed && !in.left_pressed;
        p.ctrl = in.key_ctrl;
        p.shift = in.shift;
        p.alt = in.key_alt;
        if (p.left) pressed_ = b.id;
        return p;
    }
    return std::nullopt;
}

HudLook HudButtons::look(const HudButton& b) const {
    if (!b.enabled) return HudLook::Disabled;
    if (pressed_ == b.id && holds(b.hit, px_, py_)) return HudLook::Pressed;
    if (b.toggled) return HudLook::Selected;
    const HudButton* h = hovered();
    return h && h->id == b.id ? HudLook::Hover : HudLook::Normal;
}

std::vector<std::string> hud_tooltip_lines(const data::StringTable& strings, const data::StringTable& hotkeys,
                                           const std::string& key) {
    std::vector<std::string> lines;
    const std::string* s = strings.find(key);
    if (!s || s->empty()) return lines;
    std::string cur;
    for (std::size_t i = 0; i < s->size();) {
        if ((*s)[i] != '<') {
            cur.push_back((*s)[i++]);
            continue;
        }
        const std::size_t e = s->find('>', i);
        if (e == std::string::npos) break;
        const std::string tag = s->substr(i, e - i + 1);
        i = e + 1;
        if (tag.compare(0, 3, "<n/") == 0) {
            lines.push_back(cur);
            cur.clear();
        } else if (tag.compare(0, 3, "<k ") == 0) {
            const std::size_t p = tag.find("val=\"");
            const std::size_t q = p == std::string::npos ? p : tag.find('"', p + 5);
            if (q != std::string::npos)
                if (const std::string* spec = hotkeys.find("UI/Hotkey/" + tag.substr(p + 5, q - p - 5))) cur += *spec;
        }
    }
    while (!cur.empty() && cur.back() == ' ') cur.pop_back();
    if (!cur.empty()) lines.push_back(cur);
    return lines;
}

void MeleeGame::refresh_hud_buttons(std::uint32_t view_w, std::uint32_t view_h) {
    hud_buttons_.begin_frame();
    hud_buttons_.set_pointer(mouse_x_, mouse_y_);
    if (cfg_.human_player < 0 || !view_w || !view_h) return;
    const ConsolePanel console = console_view();
    const ConsolePanel* cp = console_tex_.ok() ? &console : nullptr;
    const double sc = double(view_h) / double(kRefHeight);
    const double canvas_w = double(kRefHeight) * double(view_w) / double(view_h);
    auto px = [&](double pt) { return int(pt * sc + 0.5); };
    auto rect = [&](double x0, double y0, double w, double h) { return HudRect{px(x0), px(y0), px(x0 + w), px(y0 + h)}; };
    auto add = [&](HudButtonId id, const HudRect& r, const char* tip, const char* hotkey = "", bool enabled = true,
                   bool toggled = false, bool hover_only = false) {
        HudButton b;
        b.id = id;
        b.rect = r;
        b.tooltip_key = tip;
        b.hotkey = hotkey;
        b.enabled = enabled;
        b.toggled = toggled;
        b.hover_only = hover_only;
        hud_buttons_.add(b);
    };
    const bool art = ui_store_ && console.authored;
    if (art && console.authored->resource_icons_ok) {
        const HudAuthoredRects& a = *console.authored;
        auto span = [&](const HudAuthoredRects::R& i, const HudAuthoredRects::R& l) {
            return HudRect{px(std::min(i.x0, l.x0)), px(std::min(i.y0, l.y0)), px(std::max(i.x1, l.x1)), px(std::max(i.y1, l.y1))};
        };
        const int hr = human_race();
        add(HudButtonId::Resource0, span(a.resource_icon[0], a.resource_label[0]), "UI/ResourceToolTip0", "", true, false, true);
        add(HudButtonId::Resource1, span(a.resource_icon[1], a.resource_label[1]), "UI/ResourceToolTip1", "", true, false, true);
        add(HudButtonId::Supply, span(a.supply_icon, a.supply_label),
            hr == 1 ? "UI/ResourceSupplyToolTip_Prot" : hr == 2 ? "UI/ResourceSupplyToolTip_Zerg" : "UI/ResourceSupplyToolTip_Terr", "", true,
            false, true);
    }
    if (art) {
        add(HudButtonId::Clock, rect(290.0, 1200.0 - 302.0 - 44.0, 200.0, 44.0), "UI/MissionTimeTooltip", "", true, false, true);
    }
    add(HudButtonId::Idle, hud_idle_button_rect(view_w, view_h, cp), "UI/Hotkey/Tooltip/IdleWorker", "", idle_worker_count() > 0);
    add(HudButtonId::Army, hud_army_button_rect(view_w, view_h, cp), "UI/Hotkey/Tooltip/ArmySelect", "", army_unit_count() > 0);
    if (ready_warp_gate_count() > 0)
        add(HudButtonId::Warp, hud_warp_button_rect(view_w, view_h, cp), "UI/Hotkey/Tooltip/WarpIn");
    if (art) {
        const double bx = 395.0 - 9.0 - 42.0;
        add(HudButtonId::MinimapPing, rect(bx, 1200.0 - 229.0 - 42.0, 42.0, 42.0), "UI/PingTip", "MinimapPing");
        add(HudButtonId::MinimapTerrain, rect(bx, 1200.0 - 178.0 - 42.0, 42.0, 42.0), "UI/ToggleTerrainTip", "MinimapTerrain");
        add(HudButtonId::MinimapColor, rect(bx, 1200.0 - 127.0 - 42.0, 42.0, 42.0), "UI/ColorByPlayerTip", "MinimapColors");
        add(HudButtonId::Alliance, rect(canvas_w - 5.0 - 36.0, 5.0, 36.0, 36.0), "UI/Tooltip/AllianceButton", "", true,
            hud_ui_.topbar.alliance_open);
        const double bx0 = canvas_w - 241.0, by0 = 1200.0 - 267.0 - 56.0 + 9.0;
        add(HudButtonId::MenuHelp, rect(bx0 + 9.0, by0, 36.0, 36.0), "UI/HelpButtonToolTip", "MenuHelp");
        add(HudButtonId::MenuChat, rect(bx0 + 59.0, by0, 36.0, 36.0), "", "");
        add(HudButtonId::MenuSocial, rect(bx0 + 110.0, by0, 36.0, 36.0), "UI/SocialButtonToolTip", "MenuSocial");
        add(HudButtonId::MenuMenu, rect(canvas_w - 10.0 - 70.0, by0, 70.0, 36.0), "UI/GameMenuButtonTooltip", "MenuGame");
    }
    add(HudButtonId::Group1, hud_empty_group_tab_, "UI/ControlGroupButtonEmptyToolTip");
    for (const HudGroupButton& g : hud_group_buttons_) {
        if (g.slot < 0 || g.slot > 9) continue;
        add(HudButtonId(int(HudButtonId::Group0) + g.slot), HudRect{g.rect.x0, g.rect.y0, g.rect.x1, g.rect.y1},
            g.count > 0 ? "UI/ControlGroupButtonToolTip" : "UI/ControlGroupButtonEmptyToolTip");
    }
    if (pause_menu_open_) hud_buttons_.set_pointer(-1.0f, -1.0f);
}

void MeleeGame::open_pause_menu() {
    pause_menu_open_ = true;
    clock_.set_paused(true);
}

bool MeleeGame::on_hud_button(const HudPress& p) {
    switch (p.id) {
        case HudButtonId::Warp: {
            if (!p.left) return false;
            const std::vector<std::uint32_t> gates = ready_warp_gates();
            if (gates.empty()) return false;
            set_selection(gates);
            return true;
        }
        case HudButtonId::Army: {
            if (!p.left) return false;
            const std::vector<std::uint32_t> army = army_units();
            if (army.empty()) return false;
            set_selection(army);
            return true;
        }
        case HudButtonId::Idle:
            if (!p.left || idle_worker_count() <= 0) return false;
            if (p.ctrl && !std::getenv("OMC_HUD_BUTTONS_LEGACY")) set_selection(idle_workers());
            else select_next_idle_worker();
            return true;
        default: break;
    }
    static const bool legacy = std::getenv("OMC_HUD_BUTTONS_LEGACY") != nullptr;
    if (legacy) return false;
    if (p.id == HudButtonId::MenuMenu) {
        if (!p.left) return false;
        open_pause_menu();
        return true;
    }
    return hud_menu_on_button(p) || hud_minimap_on_button(p) || hud_selection_on_button(p) || hud_topbar_on_button(p) ||
           hud_chat_on_button(p);
}

void MeleeGame::hud_dispatch_hotkeys(const render::InputState& in) {
    static const bool legacy = std::getenv("OMC_HUD_BUTTONS_LEGACY") != nullptr;
    if (legacy || cfg_.human_player < 0 || !cat_ || cat_->hotkeys().empty()) return;
    const data::StringTable& hk = cat_->hotkeys();
    const std::vector<HudButton> buttons = hud_buttons_.all();
    for (const HudButton& b : buttons) {
        if (b.hotkey.empty() || !b.enabled || !ui_hotkey_fires(hk, b.hotkey.c_str(), in)) continue;
        HudPress p;
        p.id = b.id;
        p.left = true;
        on_hud_button(p);
    }
    render::InputState bare = in;
    bare.key_ctrl = false;
    if (in.key_ctrl && idle_worker_count() > 0 && ui_hotkey_fires(hk, "IdleWorker", bare)) set_selection(idle_workers());
    if (ui_hotkey_fires(hk, "AlertRecall", in)) alert_recall();
    hud_menu_hotkeys(in);
}

void MeleeGame::hud_ui_tick(float dt_s) {
    hud_ui_.selection.glow_t = idle_worker_count() > 0 ? hud_ui_.selection.glow_t + dt_s : 0.0f;
    std::vector<HudMinimapUi::Ping>& pings = hud_ui_.minimap.pings;
    for (HudMinimapUi::Ping& p : pings) p.age_s += dt_s;
    while (!pings.empty() && pings.front().age_s > 60.0f) pings.erase(pings.begin());
}

}
