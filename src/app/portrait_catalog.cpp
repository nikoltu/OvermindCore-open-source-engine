#include "sc2/app/portrait_catalog.hpp"

#include <algorithm>
#include <map>

#include "sc2/casc/storage.hpp"
#include "sc2/data/catalog.hpp"

namespace sc2::app {

namespace {

bool id_ok(const std::string& id) {
    if (id.empty() || id.size() > 48) return false;
    for (char c : id)
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')) return false;
    return true;
}

std::string lower(std::string s) {
    for (char& c : s)
        if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
    return s;
}

std::string words(const std::string& id) {
    std::string out;
    for (std::size_t i = 0; i < id.size(); ++i) {
        const char c = id[i];
        if (c == '_') {
            out.push_back(' ');
            continue;
        }
        const bool up = c >= 'A' && c <= 'Z';
        const bool prev_low = i > 0 && ((id[i - 1] >= 'a' && id[i - 1] <= 'z') || (id[i - 1] >= '0' && id[i - 1] <= '9'));
        if (up && prev_low) out.push_back(' ');
        out.push_back(c);
    }
    return out;
}

std::string model_file(const data::Catalog& cat, const std::string& model_id) {
    const std::optional<std::string> path = cat.scalar("CModel", model_id, "Model");
    if (!path || path->empty()) return {};
    std::string rel = cat.expand_entry_tokens("CModel", model_id, *path);
    for (char& c : rel)
        if (c == '\\') c = '/';
    return rel;
}

struct Hero {
    const char* name;
    char race;
    const char* stills[3];
};
const Hero kHeroes[] = {
    {"Portrait", 'T', {"raynorcommanderportrait_static", "raynormarineportrait_static", nullptr}},
    {"Portrait", 'T', {"tychusportrait_static", nullptr, nullptr}},
    {"Portrait", 'T', {"novaportrait_static", "smx3_nova_portrait_static", nullptr}},
    {"Portrait", 'T', {"swannportrait_static", nullptr, nullptr}},
    {"Portrait", 'T', {"hornerportrait_static", nullptr, nullptr}},
    {"Portrait", 'T', {"smx1_hanportrait_static", "hanportrait_static", nullptr}},
    {"Portrait", 'T', {"stetmannportrait_static", "stetmann_portrait_static", nullptr}},
    {"Portrait", 'T', {"toshportrait_static", nullptr, nullptr}},
    {"Portrait", 'T', {"hansonportrait_static", nullptr, nullptr}},
    {"Portrait", 'T', {"arcturusportrait_static", nullptr, nullptr}},
    {"Portrait", 'T', {"valerianportrait_static", "sm_valeriant1portrait_static", nullptr}},
    {"Portrait", 'T', {"warfieldportrait_static", nullptr, nullptr}},
    {"Portrait", 'T', {"kerriganghostportrait_static", nullptr, nullptr}},
    {"Portrait", 'Z', {"kerriganportrait_static", "smx1_kerriganinfestedportrait_static", nullptr}},
    {"Portrait", 'Z', {"smx1_zagaraportrait_static", "zagaraportrait_static", nullptr}},
    {"Portrait", 'Z', {"dehakaportrait_static", nullptr, nullptr}},
    {"Portrait", 'Z', {"smx1_evolutionmasterportrait_static", "smx1_evolutionmaster_static", "evolutionmasterportrait_static"}},
    {"Portrait", 'Z', {"smx1_infestedstukovportrait_static", nullptr, nullptr}},
    {"Portrait", 'Z', {"overmindportrait_static", nullptr, nullptr}},
    {"Portrait", 'P', {"zeratulportrait_static", nullptr, nullptr}},
    {"Portrait", 'P', {"artanisportrait_static", "artanisex2portrait_static", nullptr}},
    {"Portrait", 'P', {"karaxportrait_static", "karaxex2portrait_static", nullptr}},
    {"Portrait", 'P', {"fenixportrait_static", nullptr, nullptr}},
    {"Portrait", 'P', {"vorazunportrait_static", "vorazunex2portrait_static", nullptr}},
    {"Portrait", 'P', {"alarakportrait_static", nullptr, nullptr}},
    {"Portrait", 'P', {"tassadarportrait_static", nullptr, nullptr}},
    {"Portrait", 'P', {"selendisportrait_static", nullptr, nullptr}},
    {"Portrait", 'P', {"urunportrait_static", nullptr, nullptr}},
    {"Portrait", 'P', {"mohandarportrait_static", nullptr, nullptr}},
    {"Portrait", 'N', {"amonportrait_static", nullptr, nullptr}},
    {"Portrait", 'N', {"ourosportrait_static", nullptr, nullptr}},
    {"Portrait", 'N', {"narudportrait_static", "narudex1portrait_static", nullptr}},
};
const char* const kHeroRoots[] = {
    "campaigns/liberty.sc2campaign/base.sc2assets/Assets/Textures/", "campaigns/swarm.sc2campaign/base.sc2assets/Assets/Textures/",
    "campaigns/void.sc2campaign/base.sc2assets/Assets/Textures/",    "mods/liberty.sc2mod/base.sc2assets/Assets/Textures/",
    "mods/swarm.sc2mod/base.sc2assets/Assets/Textures/",             "mods/void.sc2mod/base.sc2assets/Assets/Textures/",
    "mods/core.sc2mod/base.sc2assets/Assets/Textures/",
};

}

PortraitRef PortraitRef::parse(const std::string& s) {
    PortraitRef r;
    const std::size_t colon = s.find(':');
    if (colon == std::string::npos) return r;
    const std::string kind = s.substr(0, colon), id = s.substr(colon + 1);
    if (!id_ok(id)) return r;
    if (kind == "unit") r.kind = Kind::Unit;
    else if (kind == "hero") r.kind = Kind::Hero;
    else return r;
    r.id = id;
    return r;
}

std::string PortraitRef::format() const {
    if (kind == Kind::Unit && id_ok(id)) return "unit:" + id;
    if (kind == Kind::Hero && id_ok(id)) return "hero:" + id;
    return {};
}

const char* PortraitCatalog::worker_unit(Race r) { return r == Race::Protoss ? "Probe" : (r == Race::Zerg ? "Drone" : "SCV"); }

std::string PortraitCatalog::unit_portrait_model(const data::Catalog& cat, const std::string& unit_id) {
    if (unit_id.empty()) return {};
    std::optional<std::string> pm = cat.scalar("CActorUnit", unit_id, "PortraitModel");
    if (!pm || pm->empty()) pm = unit_id + "Portrait";
    for (std::size_t at; (at = pm->find("##unitName##")) != std::string::npos;) pm->replace(at, 12, unit_id);
    std::string id = *pm;
    if (!cat.find("CModel", id)) return {};
    for (int depth = 0; depth < 16; ++depth) {
        const data::Catalog::Entry* e = cat.find("CModel", id);
        if (!e || e->parent.empty()) break;
        bool own_model = false;
        for (const core::XmlNode& n : e->own)
            if (n.name == "Model") {
                own_model = true;
                break;
            }
        const data::Catalog::Entry* pe = cat.find("CModel", e->parent);
        if (own_model || !pe || pe->is_default) break;
        id = e->parent;
    }
    return id;
}

std::string PortraitCatalog::unit_texture(const data::Catalog& cat, const std::string& portrait_model_id) {
    if (portrait_model_id.empty() || !cat.find("CModel", portrait_model_id)) return {};
    const std::string file = model_file(cat, portrait_model_id);
    const std::string lf = lower(file);
    if (lf.find("assets/portraits/") == std::string::npos || lf.size() < 4 || lf.compare(lf.size() - 3, 3, ".m3") != 0) return {};
    if (lf.find("portraittransition") != std::string::npos) return {};
    const std::size_t slash = lf.find_last_of('/');
    return lf.substr(slash + 1, lf.size() - slash - 4) + "_static";
}

const IconAtlas::Icon* PortraitCatalog::texture(casc::Storage& store, const std::string& name) {
    if (name.empty()) return nullptr;
    const IconAtlas::Icon& ic = atlas_.get(store, name);
    return ic.ok() ? &ic : nullptr;
}

const IconAtlas::Icon* PortraitCatalog::image(const data::Catalog& cat, casc::Storage& store, const PortraitRef& ref, Race race) {
    if (ref.kind == PortraitRef::Kind::Unit) {
        if (const IconAtlas::Icon* ic = texture(store, unit_texture(cat, ref.id))) return ic;
    } else if (ref.kind == PortraitRef::Kind::Hero) {
        for (const char* root : kHeroRoots)
            if (const IconAtlas::Icon* ic = texture(store, std::string(root) + ref.id + ".dds")) return ic;
    }
    return texture(store, unit_texture(cat, unit_portrait_model(cat, worker_unit(race))));
}

const IconAtlas::Icon* PortraitCatalog::ai_avatar(casc::Storage& store) { return texture(store, "ai_avatar"); }

const IconAtlas::Icon* PortraitCatalog::race_mark(casc::Storage& store, LobbyRace race) {
    const char* word = race == LobbyRace::Protoss ? "protoss" : (race == LobbyRace::Zerg ? "zerg" : (race == LobbyRace::Random ? "random" : "terran"));
    return texture(store, std::string("sc2_ui_loadingscreen_playercard_raceindicator_") + word);
}

void PortraitCatalog::plan(const data::Catalog& cat) {
    planned_ = true;
    todo_.clear();
    struct Group {
        std::vector<std::string> units;
        std::string texture;
    };
    std::map<std::string, Group> by_model;
    std::vector<std::string> ids = cat.ids("CUnit");
    std::sort(ids.begin(), ids.end());
    for (const std::string& uid : ids) {
        const std::string model = unit_portrait_model(cat, uid);
        if (model.empty() || !id_ok(model)) continue;
        Group& g = by_model[model];
        if (g.units.empty()) g.texture = unit_texture(cat, model);
        g.units.push_back(uid);
    }
    for (const auto& kv : by_model) {
        const Group& g = kv.second;
        if (g.texture.empty()) continue;
        const std::string stem = g.texture.substr(0, g.texture.size() - 7);
        std::string lead;
        for (const std::string& u : g.units)
            if (lower(u) + "portrait" == stem) lead = u;
        if (lead.empty()) lead = *std::min_element(g.units.begin(), g.units.end(), [](const std::string& a, const std::string& b) {
            return a.size() != b.size() ? a.size() < b.size() : a < b;
        });
        Todo t;
        t.id = kv.first;
        t.name = words(lead);
        t.texture = g.texture;
        const std::string race = cat.scalar("CUnit", lead, "Race").value_or("");
        t.race = race == "Terr" ? 'T' : (race == "Prot" ? 'P' : (race == "Zerg" ? 'Z' : 'N'));
        todo_.push_back(std::move(t));
    }
    std::sort(todo_.begin(), todo_.end(), [](const Todo& a, const Todo& b) { return a.name < b.name; });
    for (const Hero& h : kHeroes) {
        Todo t;
        t.hero = true;
        t.name = h.name;
        t.race = h.race;
        for (const char* s : h.stills) {
            if (!s) break;
            if (!t.id.empty()) t.id.push_back('|');
            t.id += s;
        }
        todo_.push_back(std::move(t));
    }
}

bool PortraitCatalog::scan(const data::Catalog& cat, casc::Storage& store, int budget) {
    if (scan_done_) return true;
    if (!planned_) plan(cat);
    for (; cursor_ < todo_.size() && budget > 0; ++cursor_, --budget) {
        const Todo& t = todo_[cursor_];
        if (!t.hero) {
            if (const IconAtlas::Icon* ic = texture(store, t.texture)) {
                Entry e;
                e.ref.kind = PortraitRef::Kind::Unit;
                e.ref.id = t.id;
                e.name = t.name;
                e.race = t.race;
                e.texture = t.texture;
                e.w = ic->w;
                e.h = ic->h;
                units_.push_back(std::move(e));
            }
            continue;
        }
        bool found = false;
        std::size_t start = 0;
        while (!found && start <= t.id.size()) {
            std::size_t bar = t.id.find('|', start);
            if (bar == std::string::npos) bar = t.id.size();
            const std::string stem = t.id.substr(start, bar - start);
            start = bar + 1;
            for (const char* root : kHeroRoots) {
                const std::string path = std::string(root) + stem + ".dds";
                if (const IconAtlas::Icon* ic = texture(store, path)) {
                    Entry e;
                    e.ref.kind = PortraitRef::Kind::Hero;
                    e.ref.id = stem;
                    e.name = t.name;
                    e.race = t.race;
                    e.texture = path;
                    e.w = ic->w;
                    e.h = ic->h;
                    heroes_.push_back(std::move(e));
                    found = true;
                    break;
                }
            }
        }
    }
    scan_done_ = cursor_ >= todo_.size();
    return scan_done_;
}

}
