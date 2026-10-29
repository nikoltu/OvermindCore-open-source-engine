#pragma once

#include <string>

#include "omc/data/strings.hpp"
#include "omc/render/input.hpp"

namespace omc::app {

inline bool hotkey_spec_fires(const std::string& spec, const render::InputState& in) {
    std::size_t start = 0;
    while (start <= spec.size()) {
        std::size_t comma = spec.find(',', start);
        if (comma == std::string::npos) comma = spec.size();
        const std::string alt = spec.substr(start, comma - start);
        start = comma + 1;
        if (alt.empty()) continue;
        bool ctrl = false, shift = false, altm = false;
        std::string key;
        std::size_t p = 0;
        while (p <= alt.size()) {
            std::size_t plus = alt.find('+', p);
            if (plus == std::string::npos) plus = alt.size();
            const std::string part = alt.substr(p, plus - p);
            p = plus + 1;
            if (part == "Control") ctrl = true;
            else if (part == "Shift") shift = true;
            else if (part == "Alt") altm = true;
            else key = part;
        }
        if (key.empty()) continue;
        if (ctrl != in.key_ctrl || shift != in.shift || altm != in.key_alt) continue;
        if (key.size() == 1 && key[0] >= 'A' && key[0] <= 'Z') {
            if (in.key_char == key[0]) return true;
            continue;
        }
        for (const std::string& t : in.key_tokens)
            if (t == key) return true;
    }
    return false;
}

inline bool ui_hotkey_fires(const data::StringTable& hotkeys, const char* name,
                            const render::InputState& in) {
    const std::string* spec = hotkeys.find(std::string("UI/Hotkey/") + name);
    return spec && hotkey_spec_fires(*spec, in);
}

}
