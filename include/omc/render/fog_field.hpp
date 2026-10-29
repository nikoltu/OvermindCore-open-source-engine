#pragma once
#include <cstdint>
#include <vector>

namespace omc::render {

struct FogField {
    std::uint32_t w = 0, h = 0;
    std::vector<std::uint8_t> alpha;
    float world_w = 0.0f, world_h = 0.0f;
    bool valid() const { return w > 0 && h > 0 && alpha.size() == std::size_t(w) * h && world_w > 0.0f && world_h > 0.0f; }
};

}
