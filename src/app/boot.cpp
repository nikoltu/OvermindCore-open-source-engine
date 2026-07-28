#include "sc2/app/boot.hpp"

#include <string>
#include <vector>

#include "sc2/asset/manager.hpp"
#include "sc2/casc/storage.hpp"

namespace sc2::app {

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
        static const char* kMods[] = {"core",       "liberty", "libertymulti",
                                      "swarm",      "swarmmulti", "void",
                                      "voidmulti"};
        static const char* kFiles[] = {"UnitData.xml", "WeaponData.xml",
                                       "EffectData.xml", "AbilData.xml"};
        std::vector<std::string> manifest;
        for (const char* mod : kMods)
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
