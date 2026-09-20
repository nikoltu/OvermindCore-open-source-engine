#pragma once

#include <cstdint>

#include "sc2/app/ui_canvas.hpp"

namespace sc2::casc {
class Storage;
}

namespace sc2::app {

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
    R idle_button;
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
    R equipment;
    R page_buttons[6];
    bool page_buttons_ok = false;
    R resource_icon[2];
    R resource_label[2];
    R supply_icon;
    R supply_label;
    bool resource_icons_ok = false;
};

const HudAuthoredRects* hud_authored_rects(casc::Storage& store, float aspect);

struct HudPauseRects {
    using R = HudAuthoredRects::R;
    float canvas_w = 0.0f;
    R panel, resume, quit;
    R save, load;
    bool ok = false;
};
const HudPauseRects* hud_pause_rects(casc::Storage& store, float aspect);

}
