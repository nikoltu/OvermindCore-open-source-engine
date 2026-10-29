#include "omc/app/melee_models.hpp"

#include <cstring>
#include "omc/formats/m3.hpp"
#include "omc/render/anim.hpp"
#include "omc/render/vertex_ao.hpp"
#include "omc/render/texture_decode.hpp"
#include <memory>
#include <optional>
#include <algorithm>
#include <cstdlib>

#include <cctype>
#include <cstdio>

#include <cstdint>
#include <exception>
#include <utility>
#include <vector>

#include "omc/casc/storage.hpp"
#include "omc/data/actor_model.hpp"
#include "omc/data/catalog.hpp"

namespace omc::app {

namespace {

const char* const kAssetRoots[] = {
    "mods/liberty.sc2mod/base.sc2assets/",
    "mods/swarm.sc2mod/base.sc2assets/",
    "mods/void.sc2mod/base.sc2assets/",
    "mods/core.sc2mod/base.sc2assets/",
};

const char* const kStoryDataRoots[] = {
    "campaigns/liberty.sc2campaign/base.sc2data",
    "campaigns/swarm.sc2campaign/base.sc2data",
    "campaigns/void.sc2campaign/base.sc2data",
};
const char* const kStoryAssetRoots[] = {
    "campaigns/liberty.sc2campaign/base.sc2assets/",
    "campaigns/swarm.sc2campaign/base.sc2assets/",
    "campaigns/void.sc2campaign/base.sc2assets/",
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

void apply_texture_declares(casc::Storage& store, const data::Catalog& cat, const std::string& model_id,
                            const std::string& prop, formats::Model& model) {
    if (prop.empty() || model.materials.empty() || !cat.find("CModel", model_id)) return;
    auto lower = [](std::string s) { for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c))); return s; };
    struct Decl { std::string prefix; std::vector<std::string> subs; bool all = false; };
    std::vector<Decl> decls;
    for (const core::XmlNode& n : cat.resolved("CModel", model_id)) {
        if (n.name != "TextureDeclares") continue;
        Decl d;
        d.prefix = lower(n.attr_or("Prefix", ""));
        for (const core::XmlNode& c : n.children) {
            if (c.name != "Adaptions") continue;
            const std::string* s = c.attr("TriggerOnSubstring");
            if (s && !s->empty()) d.subs.push_back(lower(*s)); else d.all = true;
        }
        if (!d.prefix.empty() && (d.all || !d.subs.empty())) decls.push_back(std::move(d));
    }
    if (decls.empty()) return;
    for (formats::M3Material& mat : model.materials)
        for (formats::M3Layer& layer : mat.layers) {
            if (layer.image_path.size() < 5) continue;
            std::string p = layer.image_path;
            for (char& c : p) if (c == '\\') c = '/';
            const std::size_t slash = p.find_last_of('/');
            const std::string base = lower(slash == std::string::npos ? p : p.substr(slash + 1));
            if (base.size() < 5 || base.compare(base.size() - 4, 4, ".dds") != 0) continue;
            for (const Decl& d : decls) {
                if (base.compare(0, d.prefix.size(), d.prefix) != 0) continue;
                bool hit = d.all;
                for (const std::string& s : d.subs) hit = hit || base.find(s) != std::string::npos;
                if (!hit) continue;
                const std::string cand = p.substr(0, p.size() - 4) + "_" + prop + ".dds";
                bool found = false;
                for (const char* root : kAssetRoots)
                    if (store.resolve_name_ci(std::string(root) + cand)) { found = true; break; }
                if (found) layer.image_path = cand;
                break;
            }
        }
}

bool asset_ships(const casc::Storage& store, const std::string& rel, bool story) {
    for (const char* root : kAssetRoots)
        if (store.resolve_name_ci(std::string(root) + rel)) return true;
    if (story)
        for (const char* root : kStoryAssetRoots)
            if (store.resolve_name_ci(std::string(root) + rel)) return true;
    return false;
}
bool decode_rel(casc::Storage& store, const std::string& rel, formats::Model& out, bool story) {
    for (const char* root : kAssetRoots)
        if (try_decode(store, std::string(root) + rel, out)) return true;
    if (story)
        for (const char* root : kStoryAssetRoots)
            if (try_decode(store, std::string(root) + rel, out)) return true;
    return false;
}

void bind_story_textures(const casc::Storage& store, formats::Model& model) {
    for (formats::M3Material& mat : model.materials)
        for (formats::M3Layer& layer : mat.layers) {
            if (layer.image_path.empty()) continue;
            std::string rel = layer.image_path;
            for (char& c : rel)
                if (c == '\\') c = '/';
            if (asset_ships(store, rel, false)) continue;
            for (const char* root : kStoryAssetRoots)
                if (store.resolve_name_ci(std::string(root) + rel)) { layer.image_path = std::string(root) + rel; break; }
        }
}

void graft_required_anims(casc::Storage& store, const data::Catalog& cat, const std::string& model_id, formats::Model& model, bool story) {
    static const bool kOverride = std::getenv("OMC_REQUIRED_ANIMS_OVERRIDE") != nullptr;
    const bool had_own = !model.sequences.empty();
    if ((had_own && !kOverride) || std::getenv("OMC_NO_REQUIRED_ANIMS") || !cat.find("CModel", model_id)) return;
    for (const core::XmlNode& n : cat.resolved("CModel", model_id)) {
        if (n.name != "RequiredAnims") continue;
        const std::string* v = n.attr("value");
        if (!v || v->empty()) continue;
        std::string rel = *v;
        for (char& c : rel)
            if (c == '\\') c = '/';
        formats::Model anims;
        bool got = false;
        const auto try_root = [&](const char* root) {
            if (got) return;
            try {
                anims = formats::M3::parse(store.read_by_name(std::string(root) + rel)).decode_model();
                got = !anims.sequences.empty();
            } catch (const std::exception&) {
            }
        };
        for (const char* root : kAssetRoots) try_root(root);
        if (story)
            for (const char* root : kStoryAssetRoots) try_root(root);
        if (!got) continue;
        model.stg.resize(model.sequences.size());
        const std::uint32_t base = std::uint32_t(model.stc.size());
        for (std::size_t i = 0; i < anims.sequences.size(); ++i) {
            formats::M3STG g = i < anims.stg.size() ? anims.stg[i] : formats::M3STG{};
            for (std::uint32_t& idx : g.stc_indices) idx += base;
            std::size_t same = model.sequences.size();
            if (had_own)
                for (std::size_t j = 0; j < model.sequences.size(); ++j)
                    if (model.sequences[j].name == anims.sequences[i].name) { same = j; break; }
            if (same < model.sequences.size()) {
                model.sequences[same] = anims.sequences[i];
                model.stg[same] = std::move(g);
            } else {
                model.sequences.push_back(anims.sequences[i]);
                model.stg.push_back(std::move(g));
            }
        }
        if (std::getenv("OMC_REQUIRED_ANIMS_TRACE"))
            std::fprintf(stderr, "[reqanims] %s: %zu clips from %s (model had %s)\n", model_id.c_str(), anims.sequences.size(), rel.c_str(), had_own ? "its own" : "none");
        for (formats::M3STC& c : anims.stc) model.stc.push_back(std::move(c));
    }
}

struct UnitModelSource {
    std::string model_id;
    std::optional<std::string> path;
};
UnitModelSource unit_model_source(const casc::Storage& store, const data::Catalog& cat, const std::string& unit_token, bool story) {
    UnitModelSource out;
    const std::string token = data::unit_actor_id(cat, unit_token);
    std::optional<std::string> am = cat.scalar("CActorUnit", token, "Model");
    out.model_id = (!am || am->empty() || am->find("##unitName##") != std::string::npos) ? token : *am;
    out.path = data::resolve_unit_model(cat, token);
    if (out.path && !std::getenv("OMC_NO_UNIT_FAMILY_PATH") && cat.scalar("CModel", out.model_id, "VariationCount")) {
        const std::string& rel = *out.path;
        const std::size_t dot = rel.rfind(".m3");
        const bool numbered = dot != std::string::npos && dot >= 3 && rel[dot - 3] == '_' &&
                              std::isdigit(static_cast<unsigned char>(rel[dot - 2])) && std::isdigit(static_cast<unsigned char>(rel[dot - 1]));
        if (dot != std::string::npos && !numbered && !asset_ships(store, rel, story)) {
            const std::string fam = rel.substr(0, dot) + "_00" + rel.substr(dot);
            if (asset_ships(store, fam, story)) out.path = fam;
        }
    }
    if (std::getenv("OMC_NO_OPTION_MODELSWAP") || !cat.find("CActorUnit", token)) return out;
    for (const core::XmlNode& n : cat.resolved("CActorUnit", token)) {
        if (n.name != "On") continue;
        const std::string terms = n.attr_or("Terms", ""), send = n.attr_or("Send", "");
        if (terms.compare(0, 13, "ActorCreation") != 0 || terms.find("Option Shader Ultra") == std::string::npos) continue;
        if (send.compare(0, 10, "ModelSwap ") != 0) continue;
        const std::string swap_id = send.substr(10);
        std::optional<std::string> sp = cat.scalar("CModel", swap_id, "Model");
        if (!sp || sp->empty()) continue;
        std::string rel = cat.expand_entry_tokens("CModel", swap_id, *sp);
        for (char& c : rel)
            if (c == '\\') c = '/';
        if (cat.scalar("CModel", swap_id, "VariationCount")) {
            const std::size_t dot = rel.rfind(".m3");
            const bool numbered = dot != std::string::npos && dot >= 3 && rel[dot - 3] == '_' &&
                                  std::isdigit(static_cast<unsigned char>(rel[dot - 2])) && std::isdigit(static_cast<unsigned char>(rel[dot - 1]));
            if (dot != std::string::npos && !numbered) rel = rel.substr(0, dot) + "_00" + rel.substr(dot);
        }
        out.model_id = swap_id;
        out.path = rel;
        break;
    }
    return out;
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
    if (shared_catalog_) return *shared_catalog_;
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

data::Catalog& MeleeModels::unit_catalog(casc::Storage& store, const std::string& token, bool& story) {
    story = false;
    data::Catalog& cat = catalog(store);
    if (token.empty() || std::getenv("OMC_NO_STORY_MODELS") || cat.find("CActorUnit", data::unit_actor_id(cat, token))) return cat;
    if (!story_ready_) {
        story_ready_ = true;
        story_catalog_ = std::make_unique<data::Catalog>();
        try {
            story_catalog_->load_campaign(store, std::vector<std::string>(std::begin(kStoryDataRoots), std::end(kStoryDataRoots)));
        } catch (const std::exception&) {
        }
    }
    const bool found = story_catalog_->find("CActorUnit", token) != nullptr;
    if (std::getenv("OMC_PLACED_DROP_TRACE"))
        std::fprintf(stderr, "[story-model] %s -> %s\n", token.c_str(), found ? "campaign actor" : "none");
    if (!found) return cat;
    story = true;
    return *story_catalog_;
}

void MeleeModels::resolve_one(casc::Storage& store, const std::string& token) {
    if (!texture_prop_.empty() && !std::getenv("OMC_NO_TEXTURE_SETS")) {
        const std::string pkey = token + "@" + texture_prop_;
        if (!unit_prop_done_.count(pkey)) {
            unit_prop_done_.insert(pkey);
            bool story = false;
            data::Catalog& cat = unit_catalog(store, token, story);
            const UnitModelSource src = unit_model_source(store, cat, token, story);
            const std::string model_id = src.model_id;
            bool declares = false;
            if (cat.find("CModel", model_id))
                for (const core::XmlNode& n : cat.resolved("CModel", model_id))
                    if (n.name == "TextureDeclares") { declares = true; break; }
            if (declares) {
                if (const std::optional<std::string>& path = src.path) {
                    formats::Model m;
                    if (decode_rel(store, *path, m, story)) {
                            if (story) bind_story_textures(store, m);
                            graft_required_anims(store, cat, model_id, m, story);
                            std::string before, after;
                            for (const formats::M3Material& mat : m.materials)
                                for (const formats::M3Layer& l : mat.layers) before += l.image_path + "|";
                            apply_texture_declares(store, cat, model_id, texture_prop_, m);
                            for (const formats::M3Material& mat : m.materials)
                                for (const formats::M3Layer& l : mat.layers) after += l.image_path + "|";
                            if (after != before) models_.emplace(pkey, std::move(m));
                    }
                }
            }
        }
    }
    if (models_.count(token)) return;

    bool story = false;
    std::string model_id;
    const std::vector<std::string> candidates = unit_candidates(store, token, story, model_id);
    data::Catalog& ucat = unit_catalog(store, token, story);

    formats::Model model;
    if (render::TextureDecodePool* pool = render::TextureDecodePool::existing())
        if (std::optional<render::DecodedTexture> d = pool->take("m3tok:" + token); d && d->ok && d->model) {
            model = std::move(*d->model);
            if (story) bind_story_textures(store, model);
            graft_required_anims(store, ucat, model_id, model, story);
            models_.emplace(token, std::move(model));
            return;
        }
    for (const std::string& name : candidates) {
        if (try_decode(store, name, model)) {
            if (story) bind_story_textures(store, model);
            graft_required_anims(store, ucat, model_id, model, story);
            models_.emplace(token, std::move(model));
            break;
        }
    }
}

std::vector<std::string> MeleeModels::unit_candidates(casc::Storage& store, const std::string& token, bool& story, std::string& model_id) {
    std::vector<std::string> candidates;
    story = false;
    data::Catalog& ucat = unit_catalog(store, token, story);
    const UnitModelSource src = unit_model_source(store, ucat, token, story);
    model_id = src.model_id;
    const auto push_rel = [&](const std::string& rel) {
        for (const char* root : kAssetRoots) candidates.push_back(std::string(root) + rel);
        if (story)
            for (const char* root : kStoryAssetRoots) candidates.push_back(std::string(root) + rel);
    };
    if (src.path) push_rel(*src.path);
    if (auto path = data::resolve_unit_model(ucat, data::unit_actor_id(ucat, token))) push_rel(*path);
    for (const Fallback& fb : kFallbacks) {
        if (token != fb.token) continue;
        for (const char* suffix : fb.suffixes) {
            if (!suffix) break;
            for (const char* root : kAssetRoots)
                candidates.push_back(std::string(root) + suffix);
        }
    }
    return candidates;
}

bool MeleeModels::prefetch(casc::Storage& store, const std::string& token) {
    if (token.empty() || models_.count(token) || std::getenv("OMC_MODEL_DECODE_SYNC")) return true;
    render::TextureDecodePool& pool = render::TextureDecodePool::instance();
    const std::string key = "m3tok:" + token;
    if (pool.ready(key)) return true;
    if (pool.has(key)) return false;
    bool story = false;
    std::string model_id;
    std::vector<std::string> cands = unit_candidates(store, token, story, model_id);
    casc::Storage* st = &store;
    pool.request(key, [st, cands] {
        render::DecodedTexture d;
        formats::Model m;
        for (const std::string& name : cands)
            if (try_decode(*st, name, m)) {
                d.model = std::make_shared<formats::Model>(std::move(m));
                d.ok = true;
                break;
            }
        return d;
    });
    return false;
}

std::size_t MeleeModels::memory_bytes(std::size_t* count) const {
    auto vb = [](const auto& v) { return v.capacity() * sizeof(v[0]); };
    auto model_bytes = [&](const formats::Model& m) {
        std::size_t b = vb(m.positions) + vb(m.uvs) + vb(m.uvs1) + vb(m.vertex_colors) + vb(m.indices) + vb(m.regions) +
                        vb(m.materials) + vb(m.inverse_bind) + vb(m.vertex_bone_weights) + vb(m.vertex_bone_indices) +
                        vb(m.normals) + vb(m.tangents) + vb(m.normals_w) + vb(m.bones) + vb(m.sequences) + vb(m.attachments) +
                        vb(m.particles) + vb(m.splat_weights) + vb(m.vertex_shade) + vb(m.vertex_ao) + vb(m.splat_height);
        for (const formats::M3Material& mat : m.materials)
            for (const formats::M3Layer& l : mat.layers) b += l.inline_rgba.capacity() + l.raw_record.capacity();
        for (const auto& s : m.splat_layers) b += s.rgba.capacity();
        for (const auto& s : m.splat_normals) b += s.rgba.capacity();
        for (const formats::M3STC& s : m.stc) {
            b += vb(s.anim_ids) + vb(s.anim_refs);
            for (const std::vector<formats::M3Track>& slot : s.sd)
                for (const formats::M3Track& t : slot) b += vb(t.keys) + vb(t.values) + vb(t.events);
        }
        return b;
    };
    std::size_t n = 0, b = 0;
    for (const auto* mp : {&models_, &doodads_, &carries_, &missiles_, &impacts_, &launches_, &burn_models_, &deaths_, &splats_, &actor_splats_})
        for (const auto& kv : *mp) { ++n; b += model_bytes(kv.second); }
    for (const auto& kv : beams_) { ++n; b += model_bytes(kv.second.model); }
    for (const auto& kv : scaffolds_) { ++n; b += model_bytes(kv.second.model); }
    for (const auto& kv : portraits_) { ++n; b += model_bytes(kv.second.model); }
    if (count) *count = n;
    return b;
}

void MeleeModels::load(casc::Storage& store) {
    for (const std::string& token : type_tokens()) resolve_one(store, token);
}

const formats::Model* MeleeModels::ensure_variation(casc::Storage& store, const std::string& token, unsigned pick) {
    if (token.empty()) return nullptr;
    bool story = false;
    data::Catalog& cat = unit_catalog(store, token, story);
    const UnitModelSource src = unit_model_source(store, cat, token, story);
    if (!src.path) return nullptr;
    const std::optional<std::string> vc = cat.scalar("CModel", src.model_id, "VariationCount");
    const int count = vc ? std::atoi(vc->c_str()) : 0;
    if (count <= 1) return nullptr;
    const std::size_t dot = src.path->rfind(".m3");
    if (dot == std::string::npos || dot < 3 || (*src.path)[dot - 3] != '_') return nullptr;
    char nn[8];
    std::snprintf(nn, sizeof nn, "%02u", pick % unsigned(count));
    const std::string rel = src.path->substr(0, dot - 2) + nn + src.path->substr(dot);
    const std::string key = token + "#v" + nn + (texture_prop_.empty() ? std::string() : "@" + texture_prop_);
    if (auto it = models_.find(key); it != models_.end()) return it->second.vertex_count ? &it->second : nullptr;
    formats::Model m;
    if (decode_rel(store, rel, m, story)) {
        if (story) bind_story_textures(store, m);
        graft_required_anims(store, cat, src.model_id, m, story);
        if (!texture_prop_.empty() && !std::getenv("OMC_NO_TEXTURE_SETS")) apply_texture_declares(store, cat, src.model_id, texture_prop_, m);
    }
    const formats::Model& kept = models_.emplace(key, std::move(m)).first->second;
    return kept.vertex_count ? &kept : nullptr;
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
        static const bool kRawPath = std::getenv("OMC_DEATH_PATH_RAW") != nullptr;
        if (!kRawPath && rel.find("##") != std::string::npos) rel = cat.expand_entry_tokens("CModel", *model_id, rel);
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
    static const bool kMeshOnly = std::getenv("OMC_SCAFFOLD_MESH_ONLY") != nullptr;
    const formats::Model& sm = it->second.model;
    if (sm.vertex_count == 0 && (kMeshOnly || (sm.particles.empty() && sm.ribbons.empty()))) return nullptr;
    for (int k = 0; k < 3; ++k) scale[k] = it->second.scale[k];
    return &it->second.model;
}

static void read_portrait_cam(const formats::M3& parsed, MeleeModels::Portrait& p) {
    const formats::M3Section* cam = parsed.section_by_tag("CAM_");
    if (!cam || cam->count == 0) return;
    static const bool kCamV5Old = std::getenv("OMC_PORTRAIT_CAM_V5_OLD") != nullptr;
    const std::size_t sh = cam->version >= 5 && !kCamV5Old ? 4 : 0;
    const std::size_t base = std::size_t(cam->offset);
    if (base + 72 + sh > parsed.data.size()) return;
    auto f32 = [&](std::size_t at) {
        float v = 0.0f;
        std::memcpy(&v, &parsed.data[base + at], 4);
        return v;
    };
    std::uint32_t bone = 0;
    std::memcpy(&bone, &parsed.data[base], 4);
    const float f = f32(24), zf = f32(48 + sh), zn = f32(68 + sh);
    if (f > 0.01f && f < 3.14f) p.fov_rad = f;
    if (zn > 0.0f && zf > zn) {
        p.znear = zn;
        p.zfar = zf;
    }
    p.cam_bone = int(bone);
    const bool dof_layout = cam->version == 3 || (sh != 0 && cam->version == 5);
    if (dof_layout && base + 172 + sh <= parsed.data.size()) {
        const float fd = f32(108 + sh), fs = f32(128 + sh), fe2 = f32(148 + sh), da = f32(168 + sh);
        if (fd > 0.0f && fd < 1000.0f && fs >= 0.0f && fe2 > fs && fe2 < 1000.0f && da > 0.0f && da <= 16.0f) {
            p.dof_focus = fd;
            p.dof_start = fs;
            p.dof_end = fe2;
            p.dof_amount = da;
        }
    }
}

const MeleeModels::Portrait* MeleeModels::portrait(casc::Storage& store, const std::string& unit_id) {
    if (unit_id.empty()) return nullptr;
    if (auto it = portraits_.find(unit_id); it != portraits_.end()) return it->second.ok ? &it->second : nullptr;
    Portrait p;
    data::Catalog& cat = catalog(store);
    std::string src_id, src_rel;
    if (!std::getenv("OMC_MODEL_DECODE_SYNC") && portrait_source(store, unit_id, src_id, src_rel)) {
        const std::string& expand_id = src_id;
        const std::string& rel = src_rel;
        render::DecodedTexture pre;
        if (render::TextureDecodePool* pool = render::TextureDecodePool::existing())
            if (std::optional<render::DecodedTexture> d = pool->take("m3por:" + unit_id); d && d->ok && d->m3 && d->model) pre = std::move(*d);
        int ri = -1;
        for (const char* root : kAssetRoots) {
            ++ri;
            if (pre.ok && ri < pre.pick) continue;
            try {
                formats::M3 parsed = pre.ok && ri == pre.pick ? std::move(*pre.m3) : formats::M3::parse(store.read_by_name(std::string(root) + rel));
                read_portrait_cam(parsed, p);
                p.model = pre.ok && ri == pre.pick ? std::move(*pre.model) : parsed.decode_model();
                if (p.model.vertex_count) {
                    p.path = std::string(root) + rel;
                    p.ok = true;
                    if (cat.find("CLight", expand_id)) {
                        p.light = data::resolve_environment_light(cat, expand_id);
                        p.light.params["ExposureAsAuthored"] = 1.0;
                    }
                    break;
                }
            } catch (const std::exception&) {
            }
        }
        Portrait& slot = portraits_[unit_id];
        slot = std::move(p);
        return slot.ok ? &slot : nullptr;
    }
    std::optional<std::string> pm = cat.scalar("CActorUnit", unit_id, "PortraitModel");
    if (!pm || pm->empty()) pm = unit_id + "Portrait";
    for (std::size_t at; (at = pm->find("##unitName##")) != std::string::npos;) pm->replace(at, 12, unit_id);
    std::string expand_id = *pm;
    for (int depth = 0; depth < 16; ++depth) {
        const data::Catalog::Entry* e = cat.find("CModel", expand_id);
        if (!e || e->parent.empty()) break;
        bool own_model = false;
        for (const core::XmlNode& n : e->own)
            if (n.name == "Model") { own_model = true; break; }
        const data::Catalog::Entry* pe = cat.find("CModel", e->parent);
        if (own_model || !pe || pe->is_default) break;
        expand_id = e->parent;
    }
    if (auto path = cat.scalar("CModel", expand_id, "Model")) {
        std::string rel = cat.expand_entry_tokens("CModel", expand_id, *path);
        for (char& c : rel)
            if (c == '\\') c = '/';
        for (const char* root : kAssetRoots) {
            try {
                formats::M3 parsed = formats::M3::parse(store.read_by_name(std::string(root) + rel));
                read_portrait_cam(parsed, p);
                p.model = parsed.decode_model();
                if (p.model.vertex_count) {
                    p.path = std::string(root) + rel;
                    p.ok = true;
                    if (cat.find("CLight", expand_id)) {
                        p.light = data::resolve_environment_light(cat, expand_id);
                        p.light.params["ExposureAsAuthored"] = 1.0;
                    }
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

bool MeleeModels::portrait_source(casc::Storage& store, const std::string& unit_id, std::string& expand_id, std::string& rel) {
    data::Catalog& cat = catalog(store);
    std::optional<std::string> pm = cat.scalar("CActorUnit", unit_id, "PortraitModel");
    if (!pm || pm->empty()) pm = unit_id + "Portrait";
    for (std::size_t at; (at = pm->find("##unitName##")) != std::string::npos;) pm->replace(at, 12, unit_id);
    expand_id = *pm;
    for (int depth = 0; depth < 16; ++depth) {
        const data::Catalog::Entry* e = cat.find("CModel", expand_id);
        if (!e || e->parent.empty()) break;
        bool own_model = false;
        for (const core::XmlNode& n : e->own)
            if (n.name == "Model") { own_model = true; break; }
        const data::Catalog::Entry* pe = cat.find("CModel", e->parent);
        if (own_model || !pe || pe->is_default) break;
        expand_id = e->parent;
    }
    auto path = cat.scalar("CModel", expand_id, "Model");
    if (!path) return false;
    rel = cat.expand_entry_tokens("CModel", expand_id, *path);
    for (char& c : rel)
        if (c == '\\') c = '/';
    return true;
}

bool MeleeModels::prefetch_portrait(casc::Storage& store, const std::string& unit_id) {
    if (unit_id.empty() || portraits_.count(unit_id) || std::getenv("OMC_MODEL_DECODE_SYNC")) return true;
    render::TextureDecodePool& pool = render::TextureDecodePool::instance();
    const std::string key = "m3por:" + unit_id;
    if (pool.ready(key)) return true;
    if (pool.has(key)) return false;
    std::string expand_id, rel;
    if (!portrait_source(store, unit_id, expand_id, rel)) return true;
    casc::Storage* st = &store;
    pool.request(key, [st, rel] {
        render::DecodedTexture d;
        int ri = -1;
        for (const char* root : kAssetRoots) {
            ++ri;
            try {
                auto m3 = std::make_shared<formats::M3>(formats::M3::parse(st->read_by_name(std::string(root) + rel)));
                auto model = std::make_shared<formats::Model>(m3->decode_model());
                if (model->vertex_count) {
                    d.m3 = std::move(m3);
                    d.model = std::move(model);
                    d.pick = ri;
                    d.ok = true;
                    break;
                }
            } catch (const std::exception&) {
            }
        }
        return d;
    });
    return false;
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
        if (!std::getenv("OMC_NO_BIRTH_SPLAT"))
            for (const std::string& id : cat.ids("CActorSplat"))
                for (const core::XmlNode& n : cat.resolved("CActorSplat", id)) {
                    if (n.name != "On") continue;
                    const std::string* terms = n.attr("Terms");
                    const std::string* send = n.attr("Send");
                    if (!terms || !send || *send != "Create" || terms->compare(0, 10, "UnitBirth.") != 0) continue;
                    const std::string who = terms->substr(10, terms->find(';') == std::string::npos ? std::string::npos : terms->find(';') - 10);
                    if (!who.empty() && who != "*" && who.find("##") == std::string::npos) splat_actor_by_unit_.emplace(who, id);
                }
    }
    formats::Model model;
    auto it = splat_actor_by_unit_.find(unit_id);
    if (it != splat_actor_by_unit_.end()) {
        model = load_splat_model(store, it->second);
        const std::optional<std::string> mid = cat.scalar("CActorSplat", it->second, "Model");
        if (auto sc = cat.scalar("CModel", mid ? *mid : it->second, "ScaleMax")) {
            const float v = std::strtof(sc->c_str(), nullptr);
            if (v > 0.0f) splat_scale_by_unit_[unit_id] = v;
        }
    }
    if (model.materials.empty() && !std::getenv("OMC_NO_ACTOR_SPLAT")) {
        std::string created;
        for (const core::XmlNode& n : cat.resolved("CActorUnit", unit_id)) {
            if (n.name != "On") continue;
            const std::string* terms = n.attr("Terms");
            const std::string* send = n.attr("Send");
            if (!terms || !send || send->compare(0, 7, "Create ") != 0 || terms->compare(0, 14, "BehaviorLevel.") != 0) continue;
            const std::string target = send->substr(7);
            if (target.find("Splat") != std::string::npos && cat.find("CModel", target)) created = target;
        }
        if (!created.empty()) {
            model = load_splat_model(store, created);
            if (!model.materials.empty()) actor_created_splats_.insert(unit_id);
        }
    }
    formats::Model& slot = splats_[unit_id];
    slot = std::move(model);
    return slot.vertex_count == 0 && !slot.materials.empty() ? &slot : nullptr;
}

const MeleeModels::GroundDecal* MeleeModels::unit_ground_decal(casc::Storage& store, const std::string& unit_id) {
    if (unit_id.empty()) return nullptr;
    if (auto it = ground_decals_.find(unit_id); it != ground_decals_.end())
        return it->second.model.projectors.empty() ? nullptr : &it->second;
    GroundDecal& slot = ground_decals_[unit_id];
    data::Catalog& cat = catalog(store);
    const auto pair_of = [](const std::string& v, float* out) {
        char* end = nullptr;
        out[0] = std::strtof(v.c_str(), &end);
        if (end && *end == ',') out[1] = std::strtof(end + 1, nullptr);
    };
    for (const core::XmlNode& mn : cat.resolved("CActorUnit", unit_id)) {
        if (mn.name != "Macros") continue;
        const std::string* macro = mn.attr("value");
        if (!macro || !cat.find("CActorEventMacro", *macro)) continue;
        for (const core::XmlNode& on : cat.resolved("CActorEventMacro", *macro)) {
            if (on.name != "On") continue;
            const std::string* send = on.attr("Send");
            if (!send || send->compare(0, 7, "Create ") != 0) continue;
            const std::string actor = send->substr(7);
            if (!cat.find("CActorModel", actor)) continue;
            std::string ops;
            for (const core::XmlNode& hn : cat.resolved("CActorModel", actor))
                if (hn.name == "HostSiteOps")
                    if (const std::string* o = hn.attr("Ops")) ops = *o;
            GroundDecal g;
            bool has_offset = false;
            std::size_t p = 0;
            while (p < ops.size()) {
                std::size_t e = ops.find(' ', p);
                if (e == std::string::npos) e = ops.size();
                const std::string op = ops.substr(p, e - p);
                p = e + 1;
                if (op.empty()) continue;
                if (cat.find("CActorSiteOpForward", op)) {
                    if (auto v = cat.scalar("CActorSiteOpForward", op, "Forward")) {
                        pair_of(*v, g.forward);
                        g.has_forward = true;
                    }
                } else if (cat.find("CActorSiteOpLocalOffset", op)) {
                    if (auto v = cat.scalar("CActorSiteOpLocalOffset", op, "LocalOffset")) {
                        pair_of(*v, g.offset);
                        has_offset = true;
                    }
                }
            }
            if (!g.has_forward || !has_offset) continue;
            if (auto sc = cat.scalar("CActorModel", actor, "Scale")) {
                const float v = std::strtof(sc->c_str(), nullptr);
                if (v > 0.0f) g.scale = v;
            }
            std::optional<std::string> mid = cat.scalar("CActorModel", actor, "Model");
            if (!mid) mid = actor;
            const std::optional<std::string> path = cat.scalar("CModel", *mid, "Model");
            if (!path) continue;
            std::string rel = *path;
            for (char& c : rel)
                if (c == '\\') c = '/';
            for (const char* root : kAssetRoots) {
                try {
                    g.model = formats::M3::parse(store.read_by_name(std::string(root) + rel)).decode_model();
                } catch (const std::exception&) {
                    continue;
                }
                if (!g.model.projectors.empty()) break;
            }
            if (g.model.projectors.empty() || g.model.vertex_count != 0) continue;
            std::vector<formats::M3Projector> flat;
            for (const formats::M3Projector& pj : g.model.projectors) {
                if (pj.material_type != 3 || pj.material_index >= g.model.composites.size()) {
                    flat.push_back(pj);
                    continue;
                }
                for (const formats::M3CompositeChild& ch : g.model.composites[pj.material_index].children) {
                    if (!ch.resolved || ch.type != 1) continue;
                    formats::M3Projector c = pj;
                    c.material_type = 1;
                    c.material_index = ch.index;
                    flat.push_back(std::move(c));
                }
            }
            g.model.projectors = std::move(flat);
            if (g.model.projectors.empty()) continue;
            static const bool kRest = std::getenv("OMC_DECAL_REST_VALUES") != nullptr;
            int stand = -1;
            for (std::size_t si = 0; si < g.model.sequences.size() && stand < 0; ++si)
                if (g.model.sequences[si].name.compare(0, 5, "Stand") == 0) stand = int(si);
            if (kRest || stand < 0) {
                for (formats::M3Material& mat : g.model.materials)
                    mat.layers.erase(std::remove_if(mat.layers.begin(), mat.layers.end(),
                                                    [](const formats::M3Layer& L) { return L.usage == "AlphaMask2"; }),
                                     mat.layers.end());
            } else {
                const std::uint32_t s = std::uint32_t(stand);
                for (formats::M3Material& mat : g.model.materials)
                    for (formats::M3Layer& L : mat.layers) {
                        formats::M3Layer::AnimRefs& a = L.anim;
                        float v[3];
                        render::animated_layer_field(g.model, s, a.bright_mult_interp, a.bright_mult_flags, a.bright_mult_id, 0.0f, 1, &a.bright_mult, v);
                        a.bright_mult = v[0];
                        render::animated_layer_field(g.model, s, a.midtone_interp, a.midtone_flags, a.midtone_id, 0.0f, 1, &a.midtone_offset, v);
                        a.midtone_offset = v[0];
                        render::animated_layer_field(g.model, s, a.brightness_interp, a.brightness_flags, a.brightness_id, 0.0f, 1, &a.brightness, v);
                        a.brightness = v[0];
                        render::animated_layer_field(g.model, s, a.uv_offset_interp, a.uv_offset_flags, a.uv_offset_id, 0.0f, 2, a.uv_offset, v);
                        a.uv_offset[0] = v[0]; a.uv_offset[1] = v[1];
                        render::animated_layer_field(g.model, s, a.uv_angle_interp, a.uv_angle_flags, a.uv_angle_id, 0.0f, 3, a.uv_angle, v);
                        for (int k = 0; k < 3; ++k) a.uv_angle[k] = v[k];
                        render::animated_layer_field(g.model, s, a.uv_tiling_interp, a.uv_tiling_flags, a.uv_tiling_id, 0.0f, 2, a.uv_tiling, v);
                        a.uv_tiling[0] = v[0]; a.uv_tiling[1] = v[1];
                        if (std::getenv("OMC_SPLAT_TRACE"))
                            std::fprintf(stderr, "[decalstand] %s %s %s '%s' mult %.3f add %.3f bright %.3f uvoff %.3f,%.3f uvang %.3f tile %.3f,%.3f\n", unit_id.c_str(), mat.name.c_str(),
                                         L.usage.c_str(), L.image_path.c_str(), double(a.bright_mult), double(a.midtone_offset), double(a.brightness), double(a.uv_offset[0]),
                                         double(a.uv_offset[1]), double(a.uv_angle[2]), double(a.uv_tiling[0]), double(a.uv_tiling[1]));
                    }
            }
            slot = std::move(g);
            return &slot;
        }
    }
    return nullptr;
}

formats::Model MeleeModels::load_splat_model(casc::Storage& store, const std::string& actor_id) {
    data::Catalog& cat = catalog(store);
    formats::Model model;
    std::optional<std::string> model_id = cat.scalar("CActorSplat", actor_id, "Model");
    if (!model_id) model_id = actor_id;
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
    return model;
}

const formats::Model* MeleeModels::actor_splat(casc::Storage& store, const std::string& actor_id) {
    if (actor_id.empty()) return nullptr;
    if (auto it = actor_splats_.find(actor_id); it != actor_splats_.end())
        return it->second.vertex_count == 0 && !it->second.materials.empty() ? &it->second : nullptr;
    formats::Model& slot = actor_splats_[actor_id];
    slot = load_splat_model(store, actor_id);
    return slot.vertex_count == 0 && !slot.materials.empty() ? &slot : nullptr;
}

const formats::Model* MeleeModels::missile_model(casc::Storage& store, const std::string& weapon_id) {
    if (weapon_id.empty()) return nullptr;
    static const bool kMeshOnly = std::getenv("OMC_MISSILE_MESH_ONLY") != nullptr;
    auto usable = [&](const formats::Model& m) { return m.vertex_count > 0 || (!kMeshOnly && (!m.particles.empty() || !m.ribbons.empty())); };
    if (auto it = missiles_.find(weapon_id); it != missiles_.end())
        return usable(it->second) ? &it->second : nullptr;
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
    return usable(slot) ? &slot : nullptr;
}

const formats::Model* MeleeModels::impact_model(casc::Storage& store, const std::string& weapon_id) {
    if (weapon_id.empty()) return nullptr;
    static const bool kImpactMeshOnly = std::getenv("OMC_MISSILE_MESH_ONLY") != nullptr;
    if (auto it = impacts_.find(weapon_id); it != impacts_.end())
        return (it->second.vertex_count > 0 || (!kImpactMeshOnly && !it->second.particles.empty())) ? &it->second : nullptr;
    formats::Model model;
    data::Catalog& cat = catalog(store);
    if (auto id = cat.weapon_impact_model_id(weapon_id)) {
        if (auto path = cat.scalar("CModel", *id, "Model")) {
            std::string rel = *path;
            for (char& c : rel)
                if (c == '\\') c = '/';
            for (const char* root : kAssetRoots)
                if (try_decode(store, std::string(root) + rel, model)) break;
        }
    }
    formats::Model& slot = impacts_[weapon_id];
    slot = std::move(model);
    return (slot.vertex_count > 0 || !slot.particles.empty()) ? &slot : nullptr;
}

const formats::Model* MeleeModels::launch_model(casc::Storage& store, const std::string& weapon_id) {
    if (weapon_id.empty()) return nullptr;
    if (auto it = launches_.find(weapon_id); it != launches_.end())
        return (it->second.vertex_count > 0 || !it->second.particles.empty()) ? &it->second : nullptr;
    formats::Model model;
    data::Catalog& cat = catalog(store);
    if (auto id = cat.weapon_launch_model_id(weapon_id)) {
        if (auto path = cat.scalar("CModel", *id, "Model")) {
            std::string rel = *path;
            for (char& c : rel)
                if (c == '\\') c = '/';
            for (const char* root : kAssetRoots)
                if (try_decode(store, std::string(root) + rel, model)) break;
        }
    }
    formats::Model& slot = launches_[weapon_id];
    slot = std::move(model);
    return (slot.vertex_count > 0 || !slot.particles.empty()) ? &slot : nullptr;
}

const formats::Model* MeleeModels::cmodel(casc::Storage& store, const std::string& cmodel_id) {
    if (cmodel_id.empty()) return nullptr;
    auto mit = burn_models_.find(cmodel_id);
    if (mit == burn_models_.end()) {
        formats::Model model;
        if (auto path = catalog(store).scalar("CModel", cmodel_id, "Model")) {
            std::string rel = *path;
            if (rel.find("##") != std::string::npos) rel = catalog(store).expand_entry_tokens("CModel", cmodel_id, rel);
            for (char& c : rel)
                if (c == '\\') c = '/';
            for (const char* root : kAssetRoots)
                if (try_decode(store, std::string(root) + rel, model)) break;
        }
        mit = burn_models_.emplace(cmodel_id, std::move(model)).first;
    }
    const formats::Model& fm = mit->second;
    return (fm.vertex_count > 0 || !fm.particles.empty() || !fm.ribbons.empty()) ? &fm : nullptr;
}

const std::vector<MeleeModels::BurnState>& MeleeModels::burn_states(casc::Storage& store, const std::string& unit_id) {
    auto hit = burn_.find(unit_id);
    if (hit != burn_.end()) return hit->second;
    std::vector<BurnState> out;
    data::Catalog& cat = catalog(store);
    const std::string create_row = "UnitCreation." + unit_id;
    for (const std::string& mon : cat.ids("CActorStateMonitor")) {
        bool mine = false;
        const auto& rows = cat.resolved("CActorStateMonitor", mon);
        for (const core::XmlNode& n : rows)
            if (n.name == "On" && n.attr_or("Terms", "") == create_row && n.attr_or("Send", "") == "Create") mine = true;
        if (!mine) continue;
        for (const core::XmlNode& n : rows) {
            if (n.name != "On") continue;
            const std::string terms = n.attr_or("Terms", ""), send = n.attr_or("Send", "");
            const std::size_t sv = terms.find("StateValid ");
            if (terms.rfind("StateChange", 0) != 0 || sv == std::string::npos || terms.find("!StateValid") != std::string::npos ||
                terms.find("!StateChanged") != std::string::npos || send.rfind("Create ", 0) != 0)
                continue;
            std::string state = terms.substr(sv + 11);
            if (const std::size_t semi = state.find(';'); semi != std::string::npos) state.erase(semi);
            const std::string set_id = send.substr(7);
            if (set_id.find("Transition") != std::string::npos || !cat.find("CActorSetQueried", set_id)) continue;
            std::string target, type;
            for (const core::XmlNode& sn : cat.resolved("CActorSetQueried", set_id))
                if (sn.name == "SpawnTarget")
                    if (const std::string* v = sn.attr("value")) target = *v;
            for (const data::Catalog::Entry* e = cat.find("CActorSetQueried", set_id); e && type.empty();
                 e = e->parent.empty() ? nullptr : cat.find("CActorSetQueried", e->parent))
                if (const std::string* v = e->attr("type")) type = *v;
            const std::string tok = "##type##";
            for (std::size_t at = target.find(tok); at != std::string::npos; at = target.find(tok, at)) target.replace(at, tok.size(), type);
            if (target.empty()) continue;
            std::string model_id = target;
            if (cat.find("CActorModel", target))
                for (const core::XmlNode& mn : cat.resolved("CActorModel", target))
                    if (mn.name == "Model")
                        if (const std::string* v = mn.attr("value"); v && !v->empty() && v->find("##") == std::string::npos) model_id = *v;
            auto mit = burn_models_.find(model_id);
            if (mit == burn_models_.end()) {
                formats::Model model;
                if (auto path = cat.scalar("CModel", model_id, "Model")) {
                    std::string rel = *path;
                    for (char& c : rel)
                        if (c == '\\') c = '/';
                    for (const char* root : kAssetRoots)
                        if (try_decode(store, std::string(root) + rel, model)) break;
                }
                mit = burn_models_.emplace(model_id, std::move(model)).first;
            }
            if (mit->second.vertex_count == 0 && mit->second.particles.empty()) continue;
            bool dup = false;
            for (const BurnState& b : out) dup = dup || b.state == state;
            if (!dup) out.push_back({state, &mit->second});
        }
        if (!out.empty()) break;
    }
    return burn_.emplace(unit_id, std::move(out)).first->second;
}

MeleeModels::BeamModel MeleeModels::actor_beam_model(casc::Storage& store, const std::string& beam_actor_id) {
    BeamModel out;
    if (beam_actor_id.empty()) return out;
    auto it = beams_.find(beam_actor_id);
    if (it == beams_.end()) {
        BeamSlot slot;
        data::Catalog& cat = catalog(store);
        if (cat.find("CActorBeamSimple", beam_actor_id)) {
            std::string model_id = beam_actor_id;
            for (const core::XmlNode& n : cat.resolved("CActorBeamSimple", beam_actor_id)) {
                if (n.name == "Model")
                    if (const std::string* v = n.attr("value"); v && !v->empty()) model_id = *v;
                if (n.name == "Scale")
                    if (const std::string* v = n.attr("value"); v && !v->empty()) slot.scale = float(std::atof(v->c_str()));
            }
            const std::string tok = "##id##";
            for (std::size_t at = model_id.find(tok); at != std::string::npos; at = model_id.find(tok, at))
                model_id.replace(at, tok.size(), beam_actor_id);
            for (const data::Catalog::Entry* e = cat.find("CActorBeamSimple", beam_actor_id); e;
                 e = e->parent.empty() ? nullptr : cat.find("CActorBeamSimple", e->parent))
                if (e->parent == "BeamSimpleAnimationStyleContinuous" ||
                    e->parent == "BeamSimpleAnimationStyleContinuousClosingFull") { slot.continuous = true; break; }
            if (auto path = cat.scalar("CModel", model_id, "Model")) {
                std::string rel = *path;
                for (char& c : rel)
                    if (c == '\\') c = '/';
                for (const char* root : kAssetRoots)
                    if (try_decode(store, std::string(root) + rel, slot.model)) break;
            }
        }
        it = beams_.emplace(beam_actor_id, std::move(slot)).first;
    }
    const BeamSlot& s = it->second;
    if (s.model.vertex_count > 0 || !s.model.particles.empty() || !s.model.ribbons.empty()) {
        out.model = &s.model;
        out.continuous = s.continuous;
        out.scale = s.scale;
    }
    return out;
}

MeleeModels::BeamModel MeleeModels::beam_model(casc::Storage& store, const std::string& weapon_id) {
    if (weapon_id.empty()) return {};
    auto wit = weapon_beam_.find(weapon_id);
    if (wit == weapon_beam_.end()) {
        std::string beam;
        data::Catalog& cat = catalog(store);
        const std::string eff = cat.weapon_effect_id(weapon_id);
        for (const std::string& act : cat.ids("CActorAction")) {
            std::string attack;
            for (const data::Catalog::Entry* e = cat.find("CActorAction", act); e && attack.empty();
                 e = e->parent.empty() ? nullptr : cat.find("CActorAction", e->parent))
                if (const std::string* v = e->attr("effectAttack")) attack = *v;
            if (attack.empty() || (attack != weapon_id && attack != eff)) continue;
            std::string b;
            for (const core::XmlNode& n : cat.resolved("CActorAction", act))
                if (n.name == "Beam")
                    if (const std::string* v = n.attr("value")) b = *v;
            const std::string tok = "##id##";
            for (std::size_t at = b.find(tok); at != std::string::npos; at = b.find(tok, at)) b.replace(at, tok.size(), act);
            if (!b.empty() && cat.find("CActorBeamSimple", b)) { beam = b; break; }
        }
        wit = weapon_beam_.emplace(weapon_id, beam).first;
    }
    return wit->second.empty() ? BeamModel{} : actor_beam_model(store, wit->second);
}

const formats::Model* MeleeModels::doodad_model(casc::Storage& store, const std::string& type,
                                                int variation) {
    const std::string key = doodad_key(type, variation);
    const auto drawable = [](const formats::Model& dm) {
        const bool mesh = dm.vertex_count > 0 && !(dm.regions.empty() && !dm.terrain_regions.empty());
        return mesh || !dm.particles.empty() || !dm.ribbons.empty() || !dm.lights.empty();
    };
    if (auto it = doodads_.find(key); it != doodads_.end())
        return drawable(it->second) ? &it->second : nullptr;
    formats::Model model;
    const auto unresolved = [&model] { return model.vertex_count == 0 && model.projectors.empty(); };
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
            if (unresolved() && vrel != rel)
                for (const char* root : kAssetRoots)
                    if (try_decode(store, std::string(root) + rel, model)) break;
            if (unresolved() && rel.size() > 3 &&
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
                    if (unresolved() && variation > 0)
                        for (const char* root : kAssetRoots)
                            if (try_decode(store, std::string(root) + base + "_00.m3", model)) break;
                }
            }
        }
    }
    if (unresolved()) {
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
    if (!drawable(model) && model.projectors.empty() && model.terrain_regions.empty()) doodad_misses_.insert(type);
    if (std::getenv("OMC_X_RGB_NO_CUTOUT"))
        for (formats::M3Material& mat : model.materials) {
            bool mask = false, rgb = false;
            for (const formats::M3Layer& ly : mat.layers) {
                if (ly.usage == "AlphaMask") mask = true;
                if (ly.usage == "Diffuse") rgb = !ly.raw_record.empty() && ly.word44 == 0;
            }
            if (rgb && !mask) mat.cutout_thresh = 0;
        }
    if (!std::getenv("OMC_NO_TEXTURE_SETS")) apply_texture_declares(store, cat, *model_id, texture_prop_, model);
    if (std::getenv("OMC_VERTEX_AO") && cat.find("CModel", *model_id)) {
        bool ao_data = false;
        for (const core::XmlNode& n : cat.resolved("CModel", *model_id)) {
            if (n.name != "Flags") continue;
            const std::string* ix = n.attr("index");
            const std::string* v = n.attr("value");
            if (ix && v && *ix == "AOData") ao_data = *v != "0";
        }
        if (ao_data) {
            render::VertexAoParams ap;
            if (const char* e = std::getenv("OMC_VERTEX_AO_REACH")) ap.reach = float(std::atof(e));
            if (const char* e = std::getenv("OMC_VERTEX_AO_RAYS")) ap.rays = std::atoi(e);
            if (const char* e = std::getenv("OMC_VERTEX_AO_STRENGTH")) ap.strength = float(std::atof(e));
            ap.ground = std::getenv("OMC_VERTEX_AO_GROUND") != nullptr;
            render::bake_vertex_ao(model, ap);
            if (std::getenv("OMC_VERTEX_AO_TRACE")) {
                std::size_t dark = 0;
                unsigned lo = 255;
                for (const std::uint8_t a : model.vertex_ao) { if (a < 230) ++dark; lo = std::min(lo, unsigned(a)); }
                std::fprintf(stderr, "[vao] %s verts %u baked %zu under 0.9: %zu lowest %u\n", type.c_str(), model.vertex_count, model.vertex_ao.size(), dark, lo);
            }
        }
    }
    auto [it, _] = doodads_.emplace(key, std::move(model));
    return drawable(it->second) ? &it->second : nullptr;
}

void MeleeModels::set_tileset(casc::Storage& store, const std::string& tileset) {
    texture_prop_.clear();
    if (tileset.empty()) return;
    if (std::optional<std::string> p = catalog(store).scalar("CTerrain", tileset, "TextureProp"))
        for (char c : *p) texture_prop_ += char(std::tolower(static_cast<unsigned char>(c)));
    if (!texture_prop_.empty()) {
        std::vector<std::string> loaded;
        for (const auto& kv : models_)
            if (kv.first.find('@') == std::string::npos && kv.first.find('\x1f') == std::string::npos) loaded.push_back(kv.first);
        for (const std::string& t : loaded) resolve_one(store, t);
    }
}

const formats::Model* MeleeModels::doodad_projectors(casc::Storage& store, const std::string& type, int variation) {
    doodad_model(store, type, variation);
    const std::string key = doodad_key(type, variation);
    const auto it = doodads_.find(key);
    return it != doodads_.end() && !it->second.projectors.empty() ? &it->second : nullptr;
}

const formats::Model* MeleeModels::foliage_model(casc::Storage& store, const std::string& id, int variation) {
    const std::string key = doodad_key("foliage:" + id, variation);
    if (auto it = doodads_.find(key); it != doodads_.end()) return it->second.vertex_count > 0 ? &it->second : nullptr;
    formats::Model model;
    std::string low = id;
    for (char& c : low) c = char(std::tolower(static_cast<unsigned char>(c)));
    char suffix[8];
    std::snprintf(suffix, sizeof(suffix), "_%02d", variation < 0 ? 0 : variation);
    for (const char* root : kAssetRoots)
        if (try_decode(store, std::string(root) + "assets/foliage/" + low + "/" + low + suffix + ".m3", model)) break;
    const auto it = doodads_.emplace(key, std::move(model)).first;
    return it->second.vertex_count > 0 ? &it->second : nullptr;
}

const formats::Model* MeleeModels::doodad_terrain(casc::Storage& store, const std::string& type, int variation) {
    doodad_model(store, type, variation);
    const std::string key = doodad_key(type, variation);
    const auto it = doodads_.find(key);
    return it != doodads_.end() && !it->second.terrain_regions.empty() ? &it->second : nullptr;
}

bool MeleeModels::doodad_adjusts_terrain(casc::Storage& store, const std::string& type) {
    data::Catalog& cat = catalog(store);
    std::optional<std::string> model_id = cat.scalar("CActorDoodad", type, "Model");
    if (!model_id) model_id = type;
    if (!cat.find("CModel", *model_id)) return false;
    bool on = false;
    for (const core::XmlNode& n : cat.resolved("CModel", *model_id)) {
        if (n.name != "Flags") continue;
        const std::string* ix = n.attr("index");
        const std::string* v = n.attr("value");
        if (ix && v && *ix == "AdjustTerrain") on = *v != "0";
    }
    return on;
}

const formats::Model* MeleeModels::model_for(const std::string& type_token) const {
    if (!texture_prop_.empty())
        if (auto pit = models_.find(type_token + "@" + texture_prop_); pit != models_.end()) return &pit->second;
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
