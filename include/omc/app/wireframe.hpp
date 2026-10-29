#pragma once

namespace omc::app {

enum class WireRace { Terran, Protoss, Zerg, Neutral };

struct WireColor {
    float r = 0, g = 0, b = 0;
};

inline WireColor wire_health_color(float life_frac) {
    if (life_frac > 0.6f) return {0.0f, 1.0f, 0.0f};
    if (life_frac > 0.3f) return {1.0f, 1.0f, 0.0f};
    return {1.0f, 0.0f, 0.0f};
}

struct WireframeState {
    float life_frac = 1;
    float shield_frac = 0;
    float energy_frac = 0;
    WireRace race = WireRace::Neutral;
    bool constructing = false;
    float build_progress = 1;

    WireColor color() const { return wire_health_color(life_frac); }
    bool has_shield() const { return race == WireRace::Protoss && shield_frac > 0.0f; }
    bool has_energy() const { return energy_frac > 0.0f; }
    float reveal() const { return constructing ? build_progress : 1.0f; }
};

}
