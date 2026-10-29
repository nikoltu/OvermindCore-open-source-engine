#pragma once

#include <string>
#include <vector>

namespace omc::app {

struct HudMinimapUi {
    bool show_terrain = true;
    bool ping_armed = false;
    struct Ping {
        float x = 0.0f, y = 0.0f;
        float age_s = 0.0f;
    };
    std::vector<Ping> pings;
};

struct HudSelectionUi {
    float glow_t = 0.0f;
    int alert_cycle = 0;
    int alert_count = 0;
};

struct HudTopbarUi {
    bool alliance_open = false;
};

struct HudMenuUi {
    bool paused_by_key = false;
    bool message_log_open = false;
    struct LogLine {
        std::string text;
        float game_seconds = 0.0f;
    };
    std::vector<LogLine> history;
};

struct HudChatUi {
    bool open = false;
    std::string buffer;
    struct Line {
        std::string text;
        float ttl_s = 0.0f;
    };
    std::vector<Line> lines;
};

struct HudUiState {
    HudMinimapUi minimap;
    HudSelectionUi selection;
    HudTopbarUi topbar;
    HudMenuUi menu;
    HudChatUi chat;
};

}
