#include "omc/app/boot.hpp"

#include <cstdlib>
#include <string>
#include <vector>

#include "omc/asset/manager.hpp"
#include "omc/casc/storage.hpp"
#include "omc/data/catalog.hpp"
#include "omc/data/melee_mods.hpp"

namespace omc::app {

std::vector<StartupStep> plan_startup(const std::vector<std::string>& extra_files) {
    std::vector<StartupStep> plan;
    plan.push_back({StartupStep::Kind::OpenStore, {}, "Opening game data"});
    plan.push_back({StartupStep::Kind::MeleeCatalog, {}, "Loading unit catalog"});
    for (const std::string& f : extra_files)
        plan.push_back({StartupStep::Kind::CatalogFile, f, "Loading " + f});
    for (const std::string& tok : MeleeModels::type_tokens())
        plan.push_back({StartupStep::Kind::Model, tok, "Loading " + tok});
    return plan;
}

void run_startup_step(const StartupStep& step, const std::string& data_dir, casc::Storage& store,
                      data::Catalog& cat, MeleeModels& models) {
    switch (step.kind) {
        case StartupStep::Kind::OpenStore:
            store.open(data_dir);
            break;
        case StartupStep::Kind::MeleeCatalog:
            cat.load_melee(store);
            break;
        case StartupStep::Kind::CatalogFile:
            cat.load_files(store, {step.arg});
            break;
        case StartupStep::Kind::Model:
        default:
            if (!std::getenv("OMC_MODELS_OWN_CATALOG")) models.share_catalog(&cat);
            models.ensure(store, step.arg);
            break;
    }
}

BootResult boot(const std::string& install_root) {
    BootResult r;
    r.install = probe_install(install_root);
    if (!r.install.valid) {
        r.message = "install invalid: " + r.install.message;
        return r;
    }

    try {
        casc::Storage store;
        store.open(r.install.data_dir, r.install.install_root);
        if (!store.has_mndx()) {
            r.message = "store opened but name root (MNDX) unavailable";
            return r;
        }

        asset::AssetManager assets(store);
        static const char* kFiles[] = {"UnitData.xml", "WeaponData.xml",
                                       "EffectData.xml", "AbilData.xml"};
        std::vector<std::string> manifest;
        for (const char* mod : data::kMeleeMods)
            for (const char* file : kFiles)
                manifest.push_back(std::string("mods/") + mod +
                                   ".sc2mod/base.sc2data/GameData/" + file);

        auto pw = assets.prewarm(manifest);
        r.assets_loaded = pw.loaded;
        r.asset_bytes = pw.bytes;
        r.ready = true;
        r.message = "ready: build " + r.install.build + ", prewarmed " +
                    std::to_string(pw.loaded) + " startup assets";
        return r;
    } catch (const std::exception& e) {
        r.message = std::string("boot failed: ") + e.what();
        return r;
    }
}

}
