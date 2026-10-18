#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "sc2/app/hud_render.hpp"
#include "sc2/data/strings.hpp"
#include "sc2/render/input.hpp"

namespace sc2::app {

enum class HudButtonId : std::uint8_t {
    None = 0,
    MenuHelp, MenuChat, MenuSocial, MenuMenu,
    MinimapPing, MinimapTerrain, MinimapColor,
    Alliance,
    Idle, Army, Warp,
    Group0, Group1, Group2, Group3, Group4, Group5, Group6, Group7, Group8, Group9,
    Clock, Resource0, Resource1, Supply,
    Wireframe, Portrait, Progress,
    Count
};

const char* hud_button_name(HudButtonId id);

struct HudButton {
    HudButtonId id = HudButtonId::None;
    HudRect rect{0, 0, 0, 0};
    HudRect hit{0, 0, 0, 0};
    bool enabled = true;
    bool toggled = false;
    bool hover_only = false;
    std::string tooltip_key;
    std::string hotkey;
};

struct HudPress {
    HudButtonId id = HudButtonId::None;
    bool left = false, right = false, ctrl = false, shift = false, alt = false;
};

enum class HudLook : std::uint8_t { Normal, Hover, Pressed, Selected, Disabled };
const char* hud_look_suffix(HudLook look);

class HudButtons {
public:
    void begin_frame() { buttons_.clear(); }
    void add(const HudButton& b);
    void set_pointer(float x, float y) { px_ = x; py_ = y; }

    const HudButton* find(HudButtonId id) const;
    const HudButton* at(float x, float y) const;
    const HudButton* hovered() const { return at(px_, py_); }
    HudButtonId pressed() const { return pressed_; }

    std::optional<HudPress> route(const render::InputState& in);

    HudLook look(const HudButton& b) const;
    const std::vector<HudButton>& all() const { return buttons_; }

private:
    std::vector<HudButton> buttons_;
    float px_ = -1.0f, py_ = -1.0f;
    HudButtonId pressed_ = HudButtonId::None;
};

float hud_helper_glow(float t);

std::vector<std::string> hud_tooltip_lines(const data::StringTable& strings, const data::StringTable& hotkeys,
                                           const std::string& key);

}
