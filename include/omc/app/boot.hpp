#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "omc/app/install.hpp"
#include "omc/app/melee_models.hpp"

namespace omc::casc {
class Storage;
}
namespace omc::data {
class Catalog;
}

namespace omc::app {

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
