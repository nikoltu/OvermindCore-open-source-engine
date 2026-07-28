#pragma once

#include <cstddef>
#include <string>

#include "sc2/app/install.hpp"

namespace sc2::app {

struct BootResult {
    bool ready = false;
    InstallInfo install;
    size_t assets_loaded = 0;
    size_t asset_bytes = 0;
    std::string message;
};

BootResult boot(const std::string& install_root);

}
