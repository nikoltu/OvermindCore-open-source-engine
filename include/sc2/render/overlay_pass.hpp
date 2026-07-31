#pragma once

#include <cstdint>
#include <vector>

namespace sc2::render {

std::vector<uint8_t> composite_overlay(const std::vector<uint8_t>& base_rgba,
                                       const std::vector<uint8_t>& overlay_rgba,
                                       uint32_t width, uint32_t height);

}
