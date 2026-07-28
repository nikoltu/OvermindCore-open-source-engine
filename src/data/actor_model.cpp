#include "sc2/data/actor_model.hpp"

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

std::optional<std::string> resolve_unit_model(const Catalog& cat,
                                              const std::string& unit) {
    std::optional<std::string> am = cat.scalar("CActorUnit", unit, "Model");
    std::string model_id =
        (!am || am->empty() || am->find("##unitName##") != std::string::npos) ? unit
                                                                              : *am;

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
