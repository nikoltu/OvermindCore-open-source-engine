#pragma once

#include <cstdint>

#include <string>
#include <vector>

#include "sc2/app/ui_canvas.hpp"
#include "sc2/ui/layout.hpp"

namespace sc2::casc {
class Storage;
}

namespace sc2::app {

struct HudMessageTiming {
    float duration_ms = 0;
    float fade_ms = 0;
    int max_messages = 0;
    bool ok() const { return duration_ms > 0.0f; }
};

struct HudAuthoredRects {
    bool ok = false;
    float canvas_w = 0;
    struct R {
        float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
        bool ok() const { return x1 > x0 && y1 > y0; }
    };
    R minimap;
    R minimap_panel;
    R control_group;
    R alert;
    R error_display;
    R warning_display;
    R idle_button;
    HudMessageTiming error_timing;
    HudMessageTiming warning_timing;
    std::string style_unit_name, style_unit_type, style_unit_info;
    std::string style_group_count, style_group_index;
    R vital_shield, vital_life, vital_energy;
    std::string style_vital;
    R army_button;
    R warp_button;
    R command;
    R info;
    R wireframe;
    R progress_bar, progress_button;
    bool progress_ok = false;
    R portrait;
    R resource;
    R buttons[15];
    bool buttons_ok = false;
    R group_slots[24];
    bool group_slots_ok = false;
    R queue_slots[8];
    bool queue_slots_ok = false;
    R cargo_slots[8];
    bool cargo_slots_ok = false;
    R behavior_slots[6];
    bool behavior_slots_ok = false;
    R equipment;
    R page_buttons[6];
    bool page_buttons_ok = false;
    R resource_icon[2];
    R resource_label[2];
    R supply_icon;
    R supply_label;
    bool resource_icons_ok = false;
};

struct MeleeGameUiSolve {
    ui::Frame root;
    std::vector<ui::SolvedFrame> frames;
    float canvas_w = 0.0f;
    float canvas_h = 0.0f;
};
const MeleeGameUiSolve* melee_gameui_solve(casc::Storage& store, float aspect);

const HudAuthoredRects* hud_authored_rects(casc::Storage& store, float aspect);

struct HudPauseRects {
    using R = HudAuthoredRects::R;
    float canvas_w = 0.0f;
    R panel, resume, quit;
    R save, load;
    bool ok = false;
};
const HudPauseRects* hud_pause_rects(casc::Storage& store, float aspect);

bool hud_console_color(casc::Storage& store, int race, const std::string& name, int rgb[3]);

}
