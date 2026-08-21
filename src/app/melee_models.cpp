#include "sc2/app/melee_models.hpp"

#include <cctype>
#include <cstdio>

#include <cstdint>
#include <exception>
#include <utility>
#include <vector>

#include "sc2/casc/storage.hpp"
#include "sc2/data/actor_model.hpp"
#include "sc2/data/catalog.hpp"

namespace sc2::app {

namespace {

const char* const kAssetRoots[] = {
    "mods/liberty.sc2mod/base.sc2assets/",
    "mods/swarm.sc2mod/base.sc2assets/",
    "mods/void.sc2mod/base.sc2assets/",
    "mods/core.sc2mod/base.sc2assets/",
};

struct Fallback {
    const char* token;
    const char* suffixes[3];
};
const Fallback kFallbacks[] = {
    {"CommandCenter",
     {"Assets/Buildings/Terran/CommandCenterEx1/CommandCenterEx1.m3",
      "Assets/Buildings/Terran/CommandCenter/CommandCenter.m3", nullptr}},
    {"SCV", {"Assets/Units/Terran/SCV/SCV.m3", nullptr, nullptr}},
    {"Barracks",
     {"Assets/Buildings/Terran/Barracks/Barracks.m3", nullptr, nullptr}},
    {"Marine", {"Assets/Units/Terran/Marine/Marine.m3", nullptr, nullptr}},
    {"MineralField",
     {"Assets/Buildings/Resources/CrystalsLow/CrystalsLow_00.m3",
      "Assets/Buildings/Resources/Crystals/Crystals_00.m3", nullptr}},
    {"VespeneGeyser",
     {"Assets/Buildings/Resources/VespeneGeyser_Ex2/VespeneGeyser_Ex2_Low.m3",
      "Assets/Buildings/Resources/VespeneGeyser/VespeneGeyser.m3", nullptr}},
    {"SupplyDepot", {"Assets/Buildings/Terran/SupplyDepot/SupplyDepot.m3", nullptr, nullptr}},
    {"Marauder", {"Assets/Units/Terran/Marauder/Marauder.m3", nullptr, nullptr}},
    {"Nexus", {"Assets/Buildings/Protoss/Nexus/Nexus.m3", nullptr, nullptr}},
    {"Pylon", {"Assets/Buildings/Protoss/Pylon/Pylon.m3", nullptr, nullptr}},
    {"Probe", {"Assets/Units/Protoss/Probe/Probe.m3", nullptr, nullptr}},
    {"Zealot", {"Assets/Units/Protoss/Zealot/Zealot.m3", nullptr, nullptr}},
    {"Stalker", {"Assets/Units/Protoss/Stalker/Stalker.m3", nullptr, nullptr}},
    {"Hatchery", {"Assets/Buildings/Zerg/Hatchery/Hatchery.m3", nullptr, nullptr}},
    {"Drone", {"Assets/Units/Zerg/Drone/Drone.m3", nullptr, nullptr}},
    {"Zergling", {"Assets/Units/Zerg/Zergling/Zergling.m3", nullptr, nullptr}},
    {"Roach", {"Assets/Units/Zerg/Roach/Roach.m3", nullptr, nullptr}},
};

bool try_decode(casc::Storage& store, const std::string& name, formats::Model& out) {
    try {
        std::vector<uint8_t> bytes = store.read_by_name(name);
        out = formats::M3::parse(std::move(bytes)).decode_model();
        return out.vertex_count > 0 && !out.indices.empty();
    } catch (const std::exception&) {
        return false;
    }
}

}

const std::vector<std::string>& MeleeModels::type_tokens() {
    static const std::vector<std::string> kTokens = {
        "CommandCenter", "SCV", "Barracks", "Marine", "MineralField", "VespeneGeyser"};
    return kTokens;
}

MeleeModels::MeleeModels() = default;
MeleeModels::~MeleeModels() = default;

data::Catalog& MeleeModels::catalog(casc::Storage& store) {
    if (!catalog_ready_) {
        catalog_ = std::make_unique<data::Catalog>();
        try {
            catalog_->load_melee(store);
        } catch (const std::exception&) {
        }
        catalog_ready_ = true;
    }
    return *catalog_;
}

void MeleeModels::resolve_one(casc::Storage& store, const std::string& token) {
    if (models_.count(token)) return;

    std::vector<std::string> candidates;
    if (auto path = data::resolve_unit_model(catalog(store), token)) {
        for (const char* root : kAssetRoots)
            candidates.push_back(std::string(root) + *path);
    }
    for (const Fallback& fb : kFallbacks) {
        if (token != fb.token) continue;
        for (const char* suffix : fb.suffixes) {
            if (!suffix) break;
            for (const char* root : kAssetRoots)
                candidates.push_back(std::string(root) + suffix);
        }
    }

    formats::Model model;
    for (const std::string& name : candidates) {
        if (try_decode(store, name, model)) {
            models_.emplace(token, std::move(model));
            break;
        }
    }
}

void MeleeModels::load(casc::Storage& store) {
    for (const std::string& token : type_tokens()) resolve_one(store, token);
}

const formats::Model* MeleeModels::ensure(casc::Storage& store, const std::string& token) {
    resolve_one(store, token);
    return model_for(token);
}

static std::string with_variation(const std::string& path, int variation) {
    if (variation <= 0) return path;
    const std::size_t dot = path.rfind(".m3");
    if (dot == std::string::npos || dot < 3) return path;
    std::size_t i = dot;
    while (i > 0 && std::isdigit(static_cast<unsigned char>(path[i - 1]))) --i;
    if (i == dot || i == 0 || path[i - 1] != '_') return path;
    char buf[8];
    std::snprintf(buf, sizeof(buf), "%02d", variation);
    return path.substr(0, i) + buf + path.substr(dot);
}

const formats::Model* MeleeModels::doodad_model(casc::Storage& store, const std::string& type,
                                                int variation) {
    const std::string key = variation > 0 ? type + '#' + std::to_string(variation) : type;
    if (auto it = doodads_.find(key); it != doodads_.end())
        return it->second.vertex_count > 0 ? &it->second : nullptr;
    formats::Model model;
    data::Catalog& cat = catalog(store);
    if (std::optional<std::string> model_id = cat.scalar("CActorDoodad", type, "Model")) {
        if (std::optional<std::string> path = cat.scalar("CModel", *model_id, "Model")) {
            std::string rel = *path;
            for (char& c : rel)
                if (c == '\\') c = '/';
            const std::string vrel = with_variation(rel, variation);
            for (const char* root : kAssetRoots)
                if (try_decode(store, std::string(root) + vrel, model)) break;
            if (model.vertex_count == 0 && vrel != rel)
                for (const char* root : kAssetRoots)
                    if (try_decode(store, std::string(root) + rel, model)) break;
        }
    }
    if (model.vertex_count == 0) {
        char vsuf[16];
        std::snprintf(vsuf, sizeof(vsuf), "_%02d.m3", variation);
        for (const char* root : kAssetRoots) {
            bool got = false;
            for (const char* suf :
                 {static_cast<const char*>(vsuf), "_00.m3", ".m3", "_01.m3"}) {
                const std::string name =
                    std::string(root) + "Assets/Doodads/" + type + "/" + type + suf;
                if (try_decode(store, name, model)) { got = true; break; }
            }
            if (got) break;
        }
    }
    if (model.vertex_count == 0) doodad_misses_.insert(type);
    auto [it, _] = doodads_.emplace(key, std::move(model));
    return it->second.vertex_count > 0 ? &it->second : nullptr;
}

const formats::Model* MeleeModels::model_for(const std::string& type_token) const {
    auto it = models_.find(type_token);
    return it == models_.end() ? nullptr : &it->second;
}

std::vector<std::string> MeleeModels::resolved() const {
    std::vector<std::string> out;
    for (const std::string& token : type_tokens())
        if (models_.count(token)) out.push_back(token);
    return out;
}

}
