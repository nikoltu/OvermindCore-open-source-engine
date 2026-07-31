#pragma once

#include <cstdint>
#include <vector>

#include "sc2/render/mat4.hpp"
#include "sc2/sim/world.hpp"

namespace sc2::render {

class UnitOverlay {
public:
    static constexpr int kRingBaseRadius = 12;
    static constexpr int kRingRadiusPerWorld = 6;
    static constexpr int kRingMaxRadius = 48;
    static constexpr float kRingThickness = 1.3f;

    static constexpr int kBarWidth = 24;
    static constexpr int kBarHeight = 4;
    static constexpr int kBarOffsetY = 16;

    UnitOverlay() = default;

    std::vector<uint8_t> render(const sim::World& w, const Mat4& mvp, uint32_t view_w,
                                uint32_t view_h, const std::vector<uint32_t>& selection,
                                int viewer_player) const;
};

}
