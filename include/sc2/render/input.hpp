#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace sc2::render {

struct InputState {
    uint32_t view_w = 0, view_h = 0;

    float mouse_x = 0, mouse_y = 0;
    float mouse_dx = 0, mouse_dy = 0;
    float wheel = 0;

    bool left_down = false, left_pressed = false, left_released = false;
    bool right_down = false, right_pressed = false, right_released = false;
    bool middle_down = false;

    bool shift = false;
    bool key_pan_left = false, key_pan_right = false;
    bool key_pan_up = false, key_pan_down = false;
    bool key_orbit_left = false, key_orbit_right = false;
    bool key_train = false;
    bool key_build = false;
    bool key_ctrl = false;
    bool key_num[10] = {};
    bool key_debug = false;
    bool key_hold = false;
    bool key_stop = false;
    bool key_patrol = false;

    char key_char = 0;

    std::vector<std::string> key_tokens;
    bool key_alt = false;

    std::string text;

    bool quit = false;
};

}
