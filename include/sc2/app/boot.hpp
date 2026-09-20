#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "sc2/app/install.hpp"
#include "sc2/app/melee_models.hpp"

namespace sc2::casc {
class Storage;
}
namespace sc2::data {
class Catalog;
}

namespace sc2::app {

struct BootResult {
    bool ready = false;
    InstallInfo install;
    size_t assets_loaded = 0;
    size_t asset_bytes = 0;
    std::string message;
};

BootResult boot(const std::string& install_root);

struct StartupStep {
    enum class Kind { OpenStore, MeleeCatalog, CatalogFile, Model };
    Kind kind = Kind::OpenStore;
    std::string arg;
    std::string label;
};

std::vector<StartupStep> plan_startup(const std::vector<std::string>& extra_files);

void run_startup_step(const StartupStep& step, const std::string& data_dir, casc::Storage& store,
                      data::Catalog& cat, MeleeModels& models);

}
