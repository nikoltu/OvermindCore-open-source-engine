#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "sc2/casc/storage.hpp"

namespace sc2::app {

struct ConsoleTexture {
    std::uint32_t w = 0, h = 0;
    std::vector<std::uint8_t> rgba;
    bool ok() const { return w && h && rgba.size() == std::size_t(w) * h * 4; }
};

ConsoleTexture load_console(casc::Storage& store, int race);

ConsoleTexture load_terran_console(casc::Storage& store);

void composite_console(std::vector<std::uint8_t>& img, std::uint32_t view_w,
                       std::uint32_t view_h, const std::uint8_t* rgba, int tw, int th);

}
