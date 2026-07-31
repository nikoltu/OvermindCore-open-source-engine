#pragma once

#include <cstdint>
#include <vector>

#include "sc2/formats/m3.hpp"
#include "sc2/render/mat4.hpp"
#include "sc2/sim/nav.hpp"

namespace sc2::app {

struct SynthMap {
    struct StartLocation {
        render::Vec3 position;
        std::vector<render::Vec3> minerals;
    };

    float width = 0.0f;
    float height = 0.0f;
    std::vector<StartLocation> starts;
    formats::Model ground;
    sim::NavGrid nav;

    static constexpr int kMineralsPerBase = 8;

    static SynthMap generate(float width, float height, int subdiv = 1);

    render::Vec3 center() const { return {width * 0.5f, height * 0.5f, 0.0f}; }
};

}
