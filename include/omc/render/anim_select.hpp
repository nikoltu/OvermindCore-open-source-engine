#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace omc::render {

inline float walk_time_scale(float authored_speed, float actual_speed, bool flyer) {
    if (flyer || authored_speed <= 0.0f) return 1.0f;
    float s = actual_speed / authored_speed;
    if (s < 0.1f) s = 0.1f;
    if (s > 3.0f) s = 3.0f;
    return s;
}

inline std::vector<std::string> attack_direction_tokens(
    const std::vector<std::string>& base, float dz, float threshold = 0.5f) {
    std::vector<std::string> t = base;
    if (dz > threshold)
        t.push_back("Superior");
    else if (dz < -threshold)
        t.push_back("Inferior");
    return t;
}

inline std::vector<std::string> swing_tokens(const std::vector<std::string>& base,
                                             uint32_t swing) {
    std::vector<std::string> t = base;
    t.push_back((swing & 1u) ? "Right" : "Left");
    return t;
}

inline std::vector<std::string> death_tokens(const std::string& kind) {
    if (kind.empty()) return {"Death"};
    return {"Death", kind};
}

}
