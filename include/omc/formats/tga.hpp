#pragma once

#include <cstdint>
#include <vector>

namespace omc::formats {

struct TgaInfo {
    uint32_t width = 0, height = 0;
    uint8_t bpp = 0;
};

std::vector<uint8_t> decode_tga(const std::vector<uint8_t>& bytes, TgaInfo& out);

}
