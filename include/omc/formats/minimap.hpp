#pragma once

#include <cstdint>
#include <vector>

#include "omc/formats/terrain.hpp"

namespace omc::formats {

std::vector<uint8_t> render_minimap(const HeightMap& hm, const std::vector<uint8_t>& data,
                                    const TerrainQuantize& q, uint32_t out_w,
                                    uint32_t out_h);

}
