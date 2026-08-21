#pragma once

#include <cstdint>

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
    R command;
    R info;
    R portrait;
    R resource;
    R buttons[15];
    bool buttons_ok = false;
};

const HudAuthoredRects* hud_authored_rects(casc::Storage& store, float aspect);

}
