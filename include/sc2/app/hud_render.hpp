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

inline std::string hud_clock_text(int game_seconds) {
    if (game_seconds < 0) return {};
    const int m = game_seconds / 60, s = game_seconds % 60;
    return std::to_string(m) + (s < 10 ? ":0" : ":") + std::to_string(s);
}

HudRect hud_idle_button_rect(std::uint32_t out_w, std::uint32_t out_h, const ConsolePanel* console = nullptr);

inline bool hud_supply_blocked(int used, int cap) { return cap > 0 && used >= cap; }

constexpr std::uint8_t kSupplyBlockedR = 255, kSupplyBlockedG = 0, kSupplyBlockedB = 0;

HudRect hud_cargo_slot(std::uint32_t out_w, std::uint32_t out_h, int slot, const ConsolePanel* console = nullptr);

HudRect hud_queue_slot(std::uint32_t out_w, std::uint32_t out_h, int slot, const ConsolePanel* console = nullptr);

int hud_panel_height(std::uint32_t out_h);

int hud_panel_height(std::uint32_t out_w, std::uint32_t out_h, const ConsolePanel* console);

bool hud_covers_point(std::uint32_t out_w, std::uint32_t out_h, const ConsolePanel* console,
                      float x, float y);

constexpr int kCommandCols = 5;
constexpr int kCommandRows = 3;
HudRect hud_command_cell(std::uint32_t out_w, std::uint32_t out_h, int index,
                         const ConsolePanel* console = nullptr);

constexpr int kGroupCols = 8;
constexpr int kGroupSlots = 24;
HudRect hud_group_slot(std::uint32_t out_w, std::uint32_t out_h, int index,
                       const ConsolePanel* console = nullptr);

HudRect hud_unit_wireframe_rect(std::uint32_t out_w, std::uint32_t out_h,
                                const ConsolePanel* console = nullptr);

HudRect hud_progress_bar_rect(std::uint32_t out_w, std::uint32_t out_h,
                              const ConsolePanel* console = nullptr);

HudRect hud_unit_panel_rect(std::uint32_t out_w, std::uint32_t out_h,
                            const ConsolePanel* console = nullptr);

struct CameraFootprint {
    float x[4] = {0, 0, 0, 0};
    float y[4] = {0, 0, 0, 0};
    bool valid = false;
};

struct MinimapExtent {
    float x0 = 0.0f, y0 = 0.0f, x1 = 0.0f, y1 = 0.0f;
    bool ok() const { return x1 > x0 && y1 > y0; }
};
void hud_minimap_world_to_pixel(const HudRect& mm, const MinimapExtent& e, float wx, float wy, int& px, int& py);
void hud_minimap_pixel_to_world(const HudRect& mm, const MinimapExtent& e, float sx, float sy, float& wx, float& wy);

struct MinimapBackdrop {
    const std::vector<std::uint8_t>* terrain = nullptr;
    int tw = 0, th = 0;
    const std::vector<std::uint8_t>* fog = nullptr;
    int fw = 0, fh = 0;
    int viewer_player = -1;
    MinimapExtent extent;
};

std::vector<std::uint8_t> hud_bake_minimap_terrain(const std::vector<std::uint8_t>& level,
                                                   const std::vector<std::uint8_t>& blocked,
                                                   int gw, int gh, int out_w, int out_h);

float hud_minimap_fog_factor(int vis_state);

std::uint8_t hud_fog_veil_alpha(int vis_state);

std::vector<std::uint8_t> hud_fog_veil_cells(const std::vector<std::uint8_t>& codes, int cw,
                                             int ch);

float hud_fog_veil_alpha_at(const std::vector<std::uint8_t>& cell_alpha, int cw, int ch, float cell,
                            float origin_x, float origin_y, float wx, float wy);

void hud_fog_veil_blit(std::vector<std::uint8_t>& rgba, std::uint32_t out_w, std::uint32_t out_h,
                       const std::vector<std::uint8_t>& block_alpha, int bw, int bh, int step,
                       const std::vector<std::uint8_t>* skip = nullptr);

enum class FogVisibility : std::uint8_t { Visible = 0, Snapshot = 1, Dimmed = 2 };

bool hud_fog_body_drawn(int vis_state, FogVisibility fv);

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
