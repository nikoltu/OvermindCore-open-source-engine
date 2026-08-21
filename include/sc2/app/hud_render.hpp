#pragma once

#include <cstdint>
#include <vector>

#include "sc2/app/hud.hpp"
#include "sc2/app/hud_layout.hpp"

namespace sc2::sim {
class World;
}

namespace sc2::app {

struct HudRect {
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    int w() const { return x1 - x0; }
    int h() const { return y1 - y0; }
};

struct ConsolePanel;

HudRect hud_minimap_rect(std::uint32_t out_w, std::uint32_t out_h,
                         const ConsolePanel* console = nullptr);

int hud_panel_height(std::uint32_t out_h);

int hud_panel_height(std::uint32_t out_w, std::uint32_t out_h, const ConsolePanel* console);

constexpr int kCommandCols = 5;
constexpr int kCommandRows = 3;
HudRect hud_command_cell(std::uint32_t out_w, std::uint32_t out_h, int index,
                         const ConsolePanel* console = nullptr);

HudRect hud_unit_panel_rect(std::uint32_t out_w, std::uint32_t out_h,
                            const ConsolePanel* console = nullptr);

struct CameraFootprint {
    float x[4] = {0, 0, 0, 0};
    float y[4] = {0, 0, 0, 0};
    bool valid = false;
};

struct MinimapBackdrop {
    const std::vector<std::uint8_t>* terrain = nullptr;
    int tw = 0, th = 0;
    const std::vector<std::uint8_t>* fog = nullptr;
    int fw = 0, fh = 0;
    int viewer_player = -1;
};

std::vector<std::uint8_t> hud_bake_minimap_terrain(const std::vector<std::uint8_t>& level,
                                                   const std::vector<std::uint8_t>& blocked,
                                                   int gw, int gh, int out_w, int out_h);

float hud_minimap_fog_factor(int vis_state);

struct ConsolePanel {
    const std::vector<std::uint8_t>* rgba = nullptr;
    int tw = 0, th = 0;
    int race = 0;
    const HudAuthoredRects* authored = nullptr;
    bool ok() const {
        return rgba && tw > 0 && th > 0 &&
               rgba->size() >= std::size_t(tw) * std::size_t(th) * 4;
    }
};

class HudRender {
public:
    static std::vector<std::uint8_t> render(const HudContent& content,
                                            const sim::World& world,
                                            float map_w, float map_h,
                                            std::uint32_t out_w, std::uint32_t out_h,
                                            const CameraFootprint* cam = nullptr,
                                            const MinimapBackdrop* backdrop = nullptr,
                                            const ConsolePanel* console = nullptr);
};

}
