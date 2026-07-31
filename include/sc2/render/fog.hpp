#pragma once

#include <cstdint>
#include <vector>

#include "sc2/sim/vision.hpp"

namespace sc2::render {

class FogOverlay {
public:
    static constexpr uint8_t kVisibleAlpha = 0;
    static constexpr uint8_t kExploredAlpha = 128;
    static constexpr uint8_t kUnexploredAlpha = 255;

    FogOverlay() = default;
    explicit FogOverlay(uint8_t explored_alpha) : explored_alpha_(explored_alpha) {}

    std::vector<uint8_t> mask(const sim::VisionGrid& vis) const;

    std::vector<uint8_t> render(const sim::VisionGrid& vis, uint32_t out_w,
                                uint32_t out_h) const;

private:
    uint8_t explored_alpha_ = kExploredAlpha;
};

}
