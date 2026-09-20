#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "sc2/app/hud_messages.hpp"
#include "sc2/app/wireframe.hpp"

namespace sc2::sim {
class World;
}

namespace sc2::app {

struct HudMessageLine {
    MsgZone zone = MsgZone::Alert;
    std::string text;
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
    int idle_workers = 0;
    std::vector<EquipmentEntry> equipment;
    int lead_kills = 0;

    std::size_t selection_count = 0;
    std::size_t selection_owned = 0;
    std::string selection_summary;

    std::uint32_t lead_unit = 0xFFFFFFFFu;
    WireframeState lead_wireframe;

    std::vector<HudMessageLine> messages;
};

class HudModel {
public:
    static HudContent build(const sim::World& world, int player,
                            const std::vector<std::uint32_t>& selection);
};

}
