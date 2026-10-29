#pragma once

#include <cstdint>
#include <vector>

namespace omc::formats {

std::vector<uint8_t> write_png(const uint8_t* rgba, uint32_t width,
                               uint32_t height);
std::vector<uint8_t> write_png(const std::vector<uint8_t>& rgba, uint32_t width,
                               uint32_t height);

}
