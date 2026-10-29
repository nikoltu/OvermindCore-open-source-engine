#pragma once

#include <string>
#include <string_view>

#include "omc/render/input.hpp"

namespace omc::render {

inline bool apply_modifiers(InputState& in, std::string_view mods) {
    bool ctrl = false, shift = false, alt = false;
    std::size_t p = 0;
    while (p <= mods.size()) {
        std::size_t plus = mods.find('+', p);
        if (plus == std::string_view::npos) plus = mods.size();
        const std::string_view part = mods.substr(p, plus - p);
        p = plus + 1;
        if (part == "Control") ctrl = true;
        else if (part == "Shift") shift = true;
        else if (part == "Alt") alt = true;
        else return false;
    }
    in.key_ctrl = in.key_ctrl || ctrl;
    in.shift = in.shift || shift;
    in.key_alt = in.key_alt || alt;
    return true;
}

inline bool apply_key_spec(InputState& in, std::string_view spec) {
    const std::size_t last = spec.rfind('+');
    const std::string_view key = last == std::string_view::npos ? spec : spec.substr(last + 1);
    if (key.empty()) return false;
    InputState out = in;
    if (last != std::string_view::npos && !apply_modifiers(out, spec.substr(0, last))) return false;
    if (key == "Control" || key == "Shift" || key == "Alt") return false;
    if (key.size() == 1 && ((key[0] >= 'A' && key[0] <= 'Z') || (key[0] >= 'a' && key[0] <= 'z'))) {
        const char c = key[0] >= 'a' ? char(key[0] - 'a' + 'A') : key[0];
        out.key_char = c;
        if (c == 'T') out.key_train = true;
        if (c == 'B') out.key_build = true;
        if (c == 'H') out.key_hold = true;
        if (c == 'X') out.key_stop = true;
        if (c == 'P') out.key_patrol = true;
    } else if (key.size() == 1 && key[0] >= '0' && key[0] <= '9') {
        out.key_num[key[0] - '0'] = true;
    } else if (key == "Escape") {
        out.quit = true;
    } else {
        out.key_tokens.emplace_back(key);
    }
    in = std::move(out);
    return true;
}

}
