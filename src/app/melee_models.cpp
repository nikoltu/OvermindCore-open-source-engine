#include "sc2/app/melee_models.hpp"

#include <cstring>
#include "sc2/formats/m3.hpp"

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

const formats::Model* MeleeModels::ensure_build(casc::Storage& store, const std::string& token) {
    const std::string key = token + "\x1f" "BuildModel";
    if (auto it = models_.find(key); it != models_.end()) return &it->second;

    auto path = data::resolve_build_model(catalog(store), token);
    if (!path) return nullptr;

    formats::Model model;
    for (const char* root : kAssetRoots) {
        if (try_decode(store, std::string(root) + *path, model)) {
            return &models_.emplace(key, std::move(model)).first->second;
        }
    }
    return nullptr;
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

const formats::Model* MeleeModels::carry_model(casc::Storage& store, const std::string& node_type) {
    if (auto it = carries_.find(node_type); it != carries_.end())
        return it->second.vertex_count > 0 ? &it->second : nullptr;
    formats::Model model;
    data::Catalog& cat = catalog(store);
    std::string carry_id;
    for (const std::string& link : cat.array_links("CUnit", node_type, "BehaviorArray")) {
        const data::Catalog::Entry* be = cat.find("CBehavior", link);
        if (!be || be->tag != "CBehaviorResource") continue;
        if (auto cb = cat.scalar("CBehavior", link, "CarryResourceBehavior");
            cb && !cb->empty()) {
            carry_id = *cb;
            break;
        }
        if (auto def = cat.tag_default_scalar("CBehaviorResource", "CarryResourceBehavior");
            def && !def->empty()) {
            carry_id = *def;
            for (std::size_t at = carry_id.find("##id##"); at != std::string::npos;
                 at = carry_id.find("##id##", at))
                carry_id.replace(at, 6, link);
            if (!carry_id.empty()) break;
        }
    }
    if (!carry_id.empty()) {
        auto pth = cat.scalar("CModel", carry_id, "Model");
        if (std::optional<std::string> path = pth) {
            std::string rel = *path;
            for (char& c : rel)
                if (c == '\\') c = '/';
            for (const char* root : kAssetRoots)
                if (try_decode(store, std::string(root) + rel, model)) break;
        }
    }
    formats::Model& slot = carries_[node_type];
    slot = std::move(model);
    return slot.vertex_count > 0 ? &slot : nullptr;
}

const formats::Model* MeleeModels::death_model(casc::Storage& store, const std::string& actor_model_id) {
    if (actor_model_id.empty()) return nullptr;
    if (auto it = deaths_.find(actor_model_id); it != deaths_.end())
        return it->second.vertex_count > 0 ? &it->second : nullptr;
    formats::Model model;
    data::Catalog& cat = catalog(store);
    std::optional<std::string> model_id = cat.scalar("CActorModel", actor_model_id, "Model");
    if (!model_id) model_id = actor_model_id;
    if (auto path = cat.scalar("CModel", *model_id, "Model")) {
        std::string rel = *path;
        for (char& c : rel)
            if (c == '\\') c = '/';
        for (const char* root : kAssetRoots)
            if (try_decode(store, std::string(root) + rel, model)) break;
    }
    formats::Model& slot = deaths_[actor_model_id];
    slot = std::move(model);
    return slot.vertex_count > 0 ? &slot : nullptr;
}

const formats::Model* MeleeModels::scaffold_model(casc::Storage& store, const std::string& actor_model_id, float scale[3]) {
    if (actor_model_id.empty()) return nullptr;
    auto it = scaffolds_.find(actor_model_id);
    if (it == scaffolds_.end()) {
        Scaffold sc;
        data::Catalog& cat = catalog(store);
        if (cat.find("CActorModel", actor_model_id)) {
            std::optional<std::string> model_id = cat.scalar("CActorModel", actor_model_id, "Model");
            if (!model_id || model_id->empty()) model_id = actor_model_id;
            if (auto path = cat.scalar("CModel", *model_id, "Model")) {
                std::string rel = cat.expand_entry_tokens("CModel", *model_id, *path);
                for (char& c : rel)
                    if (c == '\\') c = '/';
                for (const char* root : kAssetRoots)
                    if (try_decode(store, std::string(root) + rel, sc.model)) break;
            }
            if (auto sm = cat.scalar("CModel", *model_id, "ScaleMax"); sm && !sm->empty()) {
                float v[3] = {1.0f, 1.0f, 1.0f};
                if (std::sscanf(sm->c_str(), "%f,%f,%f", &v[0], &v[1], &v[2]) >= 1) {
                    sc.scale[0] = v[0];
                    sc.scale[1] = v[1];
                    sc.scale[2] = v[2];
                }
            }
        }
        it = scaffolds_.emplace(actor_model_id, std::move(sc)).first;
    }
    if (it->second.model.vertex_count == 0) return nullptr;
    for (int k = 0; k < 3; ++k) scale[k] = it->second.scale[k];
    return &it->second.model;
}

const MeleeModels::Portrait* MeleeModels::portrait(casc::Storage& store, const std::string& unit_id) {
    if (unit_id.empty()) return nullptr;
    if (auto it = portraits_.find(unit_id); it != portraits_.end()) return it->second.ok ? &it->second : nullptr;
    Portrait p;
    data::Catalog& cat = catalog(store);
    std::optional<std::string> pm = cat.scalar("CActorUnit", unit_id, "PortraitModel");
    if (!pm || pm->empty()) pm = unit_id + "Portrait";
    if (auto path = cat.scalar("CModel", *pm, "Model")) {
        std::string rel = cat.expand_entry_tokens("CModel", *pm, *path);
        for (char& c : rel)
            if (c == '\\') c = '/';
        for (const char* root : kAssetRoots) {
            try {
                formats::M3 parsed = formats::M3::parse(store.read_by_name(std::string(root) + rel));
                if (const formats::M3Section* cam = parsed.section_by_tag("CAM_")) {
                    if (cam->count > 0 && std::size_t(cam->offset) + 72 <= parsed.data.size()) {
                        std::uint32_t bone = 0;
                        float f = 0.0f, zf = 0.0f, zn = 0.0f;
                        std::memcpy(&bone, &parsed.data[cam->offset], 4);
                        std::memcpy(&f, &parsed.data[cam->offset + 24], 4);
                        std::memcpy(&zf, &parsed.data[cam->offset + 48], 4);
                        std::memcpy(&zn, &parsed.data[cam->offset + 68], 4);
                        if (f > 0.01f && f < 3.14f) p.fov_rad = f;
                        if (zn > 0.0f && zf > zn) {
                            p.znear = zn;
                            p.zfar = zf;
                        }
                        p.cam_bone = int(bone);
                    }
                }
                p.model = parsed.decode_model();
                if (p.model.vertex_count) {
                    p.path = std::string(root) + rel;
                    p.ok = true;
                    break;
                }
            } catch (const std::exception&) {
            }
        }
    }
    Portrait& slot = portraits_[unit_id];
    slot = std::move(p);
    return slot.ok ? &slot : nullptr;
}

const formats::Model* MeleeModels::unit_splat(casc::Storage& store, const std::string& unit_id) {
    if (unit_id.empty()) return nullptr;
    if (auto it = splats_.find(unit_id); it != splats_.end())
        return it->second.vertex_count == 0 && !it->second.materials.empty() ? &it->second : nullptr;
    data::Catalog& cat = catalog(store);
    if (!splat_index_built_) {
        splat_index_built_ = true;
        for (const std::string& id : cat.ids("CActorSplat")) {
            const data::Catalog::Entry* e = cat.find("CActorSplat", id);
            if (!e) continue;
            const std::string* un = e->attr("unitName");
            if (!un || un->empty() || *un == "Bogus" || *un == "BogusBurrowed") continue;
            splat_actor_by_unit_.emplace(*un, id);
        }
    }
    formats::Model model;
    auto it = splat_actor_by_unit_.find(unit_id);
    if (it != splat_actor_by_unit_.end()) {
        std::optional<std::string> model_id = cat.scalar("CActorSplat", it->second, "Model");
        if (!model_id) model_id = it->second;
        if (auto path = cat.scalar("CModel", *model_id, "Model")) {
            std::string rel = *path;
            for (char& c : rel)
                if (c == '\\') c = '/';
            std::string rel00 = rel;
            if (rel00.size() > 3 && rel00.compare(rel00.size() - 3, 3, ".m3") == 0) rel00.insert(rel00.size() - 3, "_00");
            for (const char* root : kAssetRoots) {
                bool ok = false;
                for (const std::string& r : {rel, rel00}) {
                    try {
                        model = formats::M3::parse(store.read_by_name(std::string(root) + r)).decode_model();
                        ok = !model.materials.empty();
                    } catch (const std::exception&) { ok = false; }
                    if (ok) break;
                }
                if (ok) break;
            }
        }
    }
    formats::Model& slot = splats_[unit_id];
    slot = std::move(model);
    return slot.vertex_count == 0 && !slot.materials.empty() ? &slot : nullptr;
}

const formats::Model* MeleeModels::missile_model(casc::Storage& store, const std::string& weapon_id) {
    if (weapon_id.empty()) return nullptr;
    if (auto it = missiles_.find(weapon_id); it != missiles_.end())
        return it->second.vertex_count > 0 ? &it->second : nullptr;
    formats::Model model;
    data::Catalog& cat = catalog(store);
    if (auto id = cat.weapon_missile_model_id(weapon_id)) {
        if (auto path = cat.scalar("CModel", *id, "Model")) {
            std::string rel = *path;
            for (char& c : rel)
                if (c == '\\') c = '/';
            for (const char* root : kAssetRoots)
                if (try_decode(store, std::string(root) + rel, model)) break;
        }
    }
    formats::Model& slot = missiles_[weapon_id];
    slot = std::move(model);
    return slot.vertex_count > 0 ? &slot : nullptr;
}

const formats::Model* MeleeModels::doodad_model(casc::Storage& store, const std::string& type,
                                                int variation) {
    const std::string key = variation > 0 ? type + '#' + std::to_string(variation) : type;
    if (auto it = doodads_.find(key); it != doodads_.end())
        return it->second.vertex_count > 0 ? &it->second : nullptr;
    formats::Model model;
    data::Catalog& cat = catalog(store);
    std::optional<std::string> model_id = cat.scalar("CActorDoodad", type, "Model");
    if (!model_id) model_id = type;
    {
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
            if (model.vertex_count == 0 && rel.size() > 3 &&
                rel.compare(rel.size() - 3, 3, ".m3") == 0) {
                std::size_t i = rel.size() - 3;
                while (i > 0 && std::isdigit(static_cast<unsigned char>(rel[i - 1]))) --i;
                const bool already_numbered = i != rel.size() - 3 && i > 0 && rel[i - 1] == '_';
                if (!already_numbered && cat.scalar("CModel", *model_id, "VariationCount")) {
                    const std::string base = rel.substr(0, rel.size() - 3);
                    char suf[16];
                    std::snprintf(suf, sizeof(suf), "_%02d.m3", variation > 0 ? variation : 0);
                    for (const char* root : kAssetRoots)
                        if (try_decode(store, std::string(root) + base + suf, model)) break;
                    if (model.vertex_count == 0 && variation > 0)
                        for (const char* root : kAssetRoots)
                            if (try_decode(store, std::string(root) + base + "_00.m3", model)) break;
                }
            }
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
