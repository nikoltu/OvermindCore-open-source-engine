#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "omc/app/hud_messages.hpp"
#include "omc/app/wireframe.hpp"

namespace omc::sim {
class World;
}

namespace omc::app {

struct HudMessageLine {
    MsgZone zone = MsgZone::Alert;
    std::string text;
    float ttl = 0.0f;
    float ttl0 = 0.0f;
};

struct HudMessageStyle {
    bool set = false;
    std::uint8_t r = 255, g = 255, b = 255;
    bool centred = false;
    int height_pt = 0;
    bool shadow = true;
    int shadow_offset = 1;
};

struct EquipmentEntry {
    std::string icon;
    std::string line;
    int level = 0;
};

struct HudContent {
    int minerals = 0;
    int gas = 0;
    int supply_used = 0;
    int supply_max = 0;
    int game_seconds = -1;
    bool widget_art = false;
    int idle_workers = 0;
    int army_units = 0;
    int warp_gates_ready = 0;
    std::vector<EquipmentEntry> equipment;
    int lead_kills = 0;

    std::size_t selection_count = 0;
    std::size_t selection_owned = 0;
    std::string selection_summary;

    std::uint32_t lead_unit = 0xFFFFFFFFu;
    WireframeState lead_wireframe;

    std::vector<HudMessageLine> messages;

    struct MinimapPing {
        float x = 0, y = 0;
        float age = 0;
        float ping_time = 6.0f;
        std::uint8_t r = 255, g = 0, b = 0;
    };
    std::vector<MinimapPing> pings;

    struct MinimapIcon {
        float x = 0, y = 0;
        const std::vector<std::uint8_t>* rgba = nullptr;
        int tw = 0, th = 0;
        float scale = 1.0f;
        std::uint32_t unit = 0xFFFFFFFFu;
    };
    std::vector<MinimapIcon> minimap_icons;

    std::uint8_t resource_rgb[3] = {126, 191, 241};
    std::uint8_t frustum_rgb[3] = {255, 255, 255};
    std::uint8_t border_rgb[3] = {0, 0, 0};
    int border_px = 1;
    HudMessageStyle error_style;
    HudMessageStyle warning_style;
    HudMessageStyle transmission_style;
    float min_dot_pt = 3.0f;

    struct MinimapFlash {
        std::uint32_t unit = 0xFFFFFFFFu;
        std::uint8_t r = 255, g = 0, b = 0;
    };
    std::vector<MinimapFlash> minimap_flashes;

    struct MinimapTeamColor {
        std::uint8_t rgb[3] = {150, 150, 150};
        std::uint8_t sel_rgb[3] = {150, 150, 150};
        bool set = false;
    };
    std::vector<MinimapTeamColor> player_colors;
    std::vector<std::uint32_t> minimap_selected;

    std::vector<std::uint8_t> minimap_priority;

    std::vector<std::pair<float, float>> radar_blips;
    std::uint8_t blip_rgb[3] = {255, 0, 0};
    float blip_alpha = 60.0f / 255.0f;

    std::vector<std::uint8_t> minimap_hidden;
    struct MinimapSnapshot {
        std::uint32_t unit = 0xFFFFFFFFu;
        int player = -1;
        float x = 0.0f, y = 0.0f;
        float radius = 0.0f;
    };
    std::vector<MinimapSnapshot> minimap_snapshots;
    const std::vector<std::uint8_t>* minimap_creep = nullptr;
    int creep_w = 0, creep_h = 0;
    float creep_x0 = 0.0f, creep_y0 = 0.0f, creep_cell = 1.0f;
    std::uint8_t creep_rgb[3] = {0, 0, 0};
    float creep_alpha = 0.0f;
};

class HudModel {
public:
    static HudContent build(const sim::World& world, int player,
                            const std::vector<std::uint32_t>& selection);
};

}
