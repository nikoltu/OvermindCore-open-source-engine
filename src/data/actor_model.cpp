#include "sc2/data/actor_model.hpp"

#include <cstdlib>
#include <map>
#include <utility>

namespace sc2::data {

namespace {

std::string map_race(const std::string& r) {
    if (r == "Terr") return "Terran";
    if (r == "Prot") return "Protoss";
    if (r == "Zerg") return "Zerg";
    if (r == "Neut") return "Neutral";
    return r;
}

std::string prefix_of(const Catalog& cat, std::string model_id) {
    for (int i = 0; i < 32 && !model_id.empty(); ++i) {
        if (model_id == "Building") return "Buildings\\";
        if (model_id == "Unit") return "Units\\";
        const Catalog::Entry* e = cat.find("CModel", model_id);
        if (!e) break;
        model_id = e->parent;
    }
    return "Units\\";
}

void replace_all(std::string& s, const std::string& from, const std::string& to) {
    for (size_t p; (p = s.find(from)) != std::string::npos;) s.replace(p, from.size(), to);
}

}

namespace {

std::optional<std::string> model_asset(const Catalog& cat, const std::string& model_id,
                                       const std::string& unit) {
    std::optional<std::string> cm = cat.scalar("CModel", model_id, "Model");
    if (!cm || cm->empty()) return std::nullopt;
    std::string path = *cm;

    if (path.find("##Prefix##") != std::string::npos)
        replace_all(path, "##Prefix##", prefix_of(cat, model_id));
    if (path.find("##Race##") != std::string::npos) {
        std::optional<std::string> race = cat.scalar("CUnit", unit, "Race");
        replace_all(path, "##Race##", race ? map_race(*race) : "Neutral");
    }
    replace_all(path, "##id##", model_id);

    for (char& c : path)
        if (c == '\\') c = '/';
    return path;
}

}

std::string unit_actor_id(const Catalog& cat, const std::string& token) {
    static const std::string kFly = "Flying";
    if (cat.find("CActorUnit", token)) return token;
    if (!std::getenv("SC2_NO_BIRTH_ACTOR") && !token.empty()) {
        static std::map<std::pair<const Catalog*, std::string>, std::string> cache;
        const auto key = std::make_pair(&cat, token);
        if (auto it = cache.find(key); it != cache.end()) {
            if (!it->second.empty()) return it->second;
        } else {
            std::string found;
            const std::string want = "UnitBirth." + token;
            for (const std::string& id : cat.ids("CActorUnit")) {
                for (const core::XmlNode& n : cat.resolved("CActorUnit", id)) {
                    if (n.name != "On" || n.attr_or("Send", "") != "Create" || n.attr_or("Terms", "") != want) continue;
                    found = id;
                    break;
                }
                if (!found.empty()) break;
            }
            cache.emplace(key, found);
            if (!found.empty()) return found;
        }
    }
    if (token.size() <= kFly.size() || std::getenv("SC2_NO_FLYER_ACTOR")) return token;
    if (token.compare(token.size() - kFly.size(), kFly.size(), kFly) != 0) return token;
    const std::string base = token.substr(0, token.size() - kFly.size());
    if (!cat.find("CActorUnit", base)) return token;
    for (const core::XmlNode& n : cat.resolved("CActorUnit", base)) {
        if (n.name != "On" || n.attr_or("Send", "") != "Create") continue;
        const std::string terms = n.attr_or("Terms", "");
        if (terms == "UnitBirth.##unitName##Flying" || terms == "UnitBirth." + token) return base;
    }
    return token;
}

std::optional<std::string> resolve_unit_model(const Catalog& cat, const std::string& unit) {
    std::optional<std::string> am = cat.scalar("CActorUnit", unit, "Model");
    const std::string model_id =
        (!am || am->empty() || am->find("##unitName##") != std::string::npos) ? unit : *am;
    return model_asset(cat, model_id, unit);
}

std::array<float, 3> resolve_model_scale(const Catalog& cat, const std::string& model_id) {
    std::array<float, 3> out{1.0f, 1.0f, 1.0f};
    if (model_id.empty()) return out;
    auto triple = [&](const char* field, std::array<float, 3>& v) {
        std::optional<std::string> raw = cat.scalar("CModel", model_id, field);
        if (!raw || raw->empty()) return false;
        std::size_t comps = 0;
        const char* p = raw->c_str();
        while (*p && comps < 3) {
            char* end = nullptr;
            const double d = std::strtod(p, &end);
            if (end == p) break;
            v[comps++] = float(d);
            p = end;
            while (*p == ',' || *p == ' ') ++p;
        }
        if (comps == 1) v[1] = v[2] = v[0];
        return comps > 0;
    };
    std::array<float, 3> mx{1.0f, 1.0f, 1.0f}, mn{1.0f, 1.0f, 1.0f};
    const bool has_max = triple("ScaleMax", mx), has_min = triple("ScaleMin", mn);
    if (!has_max && !has_min) return out;
    if (!has_max) mx = mn;
    if (!has_min) mn = mx;
    for (int i = 0; i < 3; ++i) out[std::size_t(i)] = 0.5f * (mn[std::size_t(i)] + mx[std::size_t(i)]);
    return out;
}

std::array<float, 3> resolve_unit_model_scale(const Catalog& cat, const std::string& unit_name) {
    if (unit_name.empty()) return {1.0f, 1.0f, 1.0f};
    const std::string unit = unit_actor_id(cat, unit_name);
    std::optional<std::string> am = cat.scalar("CActorUnit", unit, "Model");
    return resolve_model_scale(cat, (!am || am->empty() || am->find("##unitName##") != std::string::npos) ? unit : *am);
}

std::array<float, 3> resolve_doodad_model_scale(const Catalog& cat, const std::string& doodad_type) {
    if (doodad_type.empty()) return {1.0f, 1.0f, 1.0f};
    std::optional<std::string> am = cat.scalar("CActorDoodad", doodad_type, "Model");
    return resolve_model_scale(cat, (!am || am->empty() || am->find("##") != std::string::npos) ? doodad_type : *am);
}

std::optional<std::string> resolve_build_model(const Catalog& cat, const std::string& unit) {
    std::optional<std::string> bm = cat.scalar("CActorUnit", unit, "BuildModel");
    if (!bm || bm->empty()) return std::nullopt;

    std::string model_id = *bm;
    replace_all(model_id, "##unitName##", unit);
    if (model_id.empty()) return std::nullopt;

    std::optional<std::string> am = cat.scalar("CActorUnit", unit, "Model");
    const std::string finished_id =
        (!am || am->empty() || am->find("##unitName##") != std::string::npos) ? unit : *am;
    if (model_id == finished_id) return std::nullopt;

    return model_asset(cat, model_id, unit);
}

}
