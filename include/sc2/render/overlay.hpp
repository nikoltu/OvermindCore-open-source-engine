#pragma once

#include <cstdint>
#include <vector>

#include "sc2/render/offscreen.hpp"

namespace sc2::render {

struct OverlayUnit {
    float world_pos[3] = {0.0f, 0.0f, 0.0f};
    float ground_radius = 0.5f;
    float hp_fraction = 1.0f;
    bool selected = false;
    float team_rgb[3] = {1.0f, 1.0f, 1.0f};
};

void draw_overlay(RenderResult& result, const std::vector<OverlayUnit>& units);

}
