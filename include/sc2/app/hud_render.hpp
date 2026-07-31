#pragma once

#include <cstdint>
#include <vector>

#include "sc2/app/hud.hpp"

namespace sc2::sim {
class World;
}

namespace sc2::app {

struct HudRect {
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    int w() const { return x1 - x0; }
    int h() const { return y1 - y0; }
};

HudRect hud_minimap_rect(std::uint32_t out_w, std::uint32_t out_h);

int hud_panel_height(std::uint32_t out_h);

constexpr int kCommandCols = 5;
constexpr int kCommandRows = 3;
HudRect hud_command_cell(std::uint32_t out_w, std::uint32_t out_h, int index);

struct CameraFootprint {
    float x[4] = {0, 0, 0, 0};
    float y[4] = {0, 0, 0, 0};
    bool valid = false;
};

class HudRender {
public:
    static std::vector<std::uint8_t> render(const HudContent& content,
                                            const sim::World& world,
                                            float map_w, float map_h,
                                            std::uint32_t out_w, std::uint32_t out_h,
                                            const CameraFootprint* cam = nullptr);
};

}
