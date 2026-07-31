#include "sc2/data/catalog.hpp"

#include <cstdlib>

#include "sc2/casc/storage.hpp"

namespace sc2::data {

using core::XmlNode;

std::string Catalog::family_of(const std::string& tag) {
    if (tag.rfind("CRequirement", 0) == 0) return "CRequirement";
    if (tag.rfind("CUnit", 0) == 0) return "CUnit";
    if (tag.rfind("CWeapon", 0) == 0) return "CWeapon";
    if (tag.rfind("CEffect", 0) == 0) return "CEffect";
    if (tag.rfind("CAbil", 0) == 0) return "CAbil";
    if (tag.rfind("CBehavior", 0) == 0) return "CBehavior";
    if (tag.rfind("CUpgrade", 0) == 0) return "CUpgrade";
    return tag;
}

std::string Catalog::expand_id(std::string v, const std::string& id) {
    for (size_t p; (p = v.find("##id##")) != std::string::npos;)
        v.replace(p, 6, id);
    return v;
}

void Catalog::ingest(const std::vector<uint8_t>& xml) {
    if (xml.empty()) return;
    XmlNode root = core::xml_parse(xml);
    if (root.name != "Catalog") return;
    for (const XmlNode& e : root.children) {
        if (e.name == "const") {
            const std::string* cid = e.attr("id");
            const std::string* cv = e.attr("value");
            if (cid && cv) consts_[*cid] = *cv;
            continue;
        }
        if (e.name.empty()) continue;
        const std::string* id = e.attr("id");
        if (!id) continue;
        std::string family = family_of(e.name);
        Entry& entry = families_[family][*id];
        entry.id = *id;
        entry.tag = e.name;
        if (const std::string* p = e.attr("parent")) entry.parent = *p;
        if (const std::string* d = e.attr("default"); d && *d == "1")
            entry.is_default = true;
        for (const auto& [k, v] : e.attrs) {
            bool found = false;
            for (auto& kv : entry.attrs)
                if (kv.first == k) { kv.second = v; found = true; break; }
            if (!found) entry.attrs.emplace_back(k, v);
        }
        for (const XmlNode& field : e.children) entry.own.push_back(field);
    }
    resolved_cache_.clear();
}

const std::string* Catalog::const_value(const std::string& id) const {
    auto it = consts_.find(id);
    return it == consts_.end() ? nullptr : &it->second;
}

const Catalog::Entry* Catalog::find(const std::string& family,
                                    const std::string& id) const {
    auto fit = families_.find(family);
    if (fit == families_.end()) return nullptr;
    auto it = fit->second.find(id);
    return it == fit->second.end() ? nullptr : &it->second;
}

std::vector<std::string> Catalog::ids(const std::string& family) const {
    std::vector<std::string> out;
    auto fit = families_.find(family);
    if (fit == families_.end()) return out;
    out.reserve(fit->second.size());
    for (const auto& [id, entry] : fit->second) out.push_back(id);
    return out;
}

const std::vector<XmlNode>& Catalog::resolved(const std::string& family,
                                              const std::string& id) const {
    std::string key = family + "\x1f" + id;
    auto cit = resolved_cache_.find(key);
    if (cit != resolved_cache_.end()) return cit->second;

    std::vector<XmlNode> out;
    const Entry* e = find(family, id);
    if (e) {
        resolved_cache_.emplace(key, std::vector<XmlNode>{});
        if (!e->parent.empty() && e->parent != id) {
            const auto& base = resolved(family, e->parent);
            out.insert(out.end(), base.begin(), base.end());
        }
        out.insert(out.end(), e->own.begin(), e->own.end());
    }
    return resolved_cache_[key] = std::move(out);
}

std::optional<std::string> Catalog::scalar(const std::string& family,
                                           const std::string& id,
                                           const std::string& field) const {
    const auto& children = resolved(family, id);
    for (auto it = children.rbegin(); it != children.rend(); ++it) {
        if (it->name != field) continue;
        if (const std::string* v = it->attr("value")) return expand_id(*v, id);
        if (const std::string* l = it->attr("Link")) return expand_id(*l, id);
        return std::string{};
    }
    return std::nullopt;
}

std::optional<double> Catalog::scalar_num(const std::string& family,
                                          const std::string& id,
                                          const std::string& field) const {
    auto s = scalar(family, id, field);
    if (!s || s->empty()) return std::nullopt;
    return std::strtod(s->c_str(), nullptr);
}

std::vector<std::string> Catalog::array_links(const std::string& family,
                                              const std::string& id,
                                              const std::string& field) const {
    const auto& children = resolved(family, id);
    std::vector<std::string> ordered;
    std::unordered_map<std::string, size_t> byidx;
    for (const XmlNode& c : children) {
        if (c.name != field) continue;
        const std::string* v = c.attr("Link");
        if (!v) v = c.attr("value");
        std::string val = v ? expand_id(*v, id) : std::string{};
        if (const std::string* idx = c.attr("index")) {
            auto it = byidx.find(*idx);
            if (it == byidx.end()) {
                byidx[*idx] = ordered.size();
                ordered.push_back(val);
            } else {
                ordered[it->second] = val;
            }
        } else {
            ordered.push_back(val);
        }
    }
    return ordered;
}

std::optional<int> Catalog::effect_damage(const std::string& effect_id,
                                          int depth) const {
    if (depth > 32) return std::nullopt;
    const Entry* e = find("CEffect", effect_id);
    if (!e) return std::nullopt;
    const std::string& tag = e->tag;

    if (tag == "CEffectDamage") {
        if (auto a = scalar_num("CEffect", effect_id, "Amount")) return int(*a);
        return std::nullopt;
    }
    auto follow = [&](const std::string& field) -> std::optional<int> {
        if (auto link = scalar("CEffect", effect_id, field); link && !link->empty())
            return effect_damage(*link, depth + 1);
        return std::nullopt;
    };
    auto follow_array = [&](const std::string& field) -> std::optional<int> {
        for (const std::string& link : array_links("CEffect", effect_id, field))
            if (!link.empty())
                if (auto d = effect_damage(link, depth + 1)) return d;
        return std::nullopt;
    };

    if (tag == "CEffectLaunchMissile") {
        if (auto d = follow("ImpactEffect")) return d;
    } else if (tag == "CEffectCreatePersistent") {
        if (auto d = follow_array("PeriodicEffectArray")) return d;
        if (auto d = follow("InitialEffect")) return d;
    } else if (tag == "CEffectSet") {
        if (auto d = follow_array("EffectArray")) return d;
    }
    for (const char* f : {"ImpactEffect", "InitialEffect", "FinalEffect"})
        if (auto d = follow(f)) return d;
    for (const char* f : {"EffectArray", "PeriodicEffectArray", "CaseArray"})
        if (auto d = follow_array(f)) return d;
    return std::nullopt;
}

std::optional<int> Catalog::weapon_damage(const std::string& weapon_id) const {
    const Entry* w = find("CWeapon", weapon_id);
    if (!w) return std::nullopt;
    std::string effect = scalar("CWeapon", weapon_id, "Effect").value_or("");
    if (effect.empty()) effect = weapon_id;
    return effect_damage(effect, 0);
}

Catalog::UnitStats Catalog::unit_stats(const std::string& unit_id) const {
    UnitStats s;
    s.life_max = scalar_num("CUnit", unit_id, "LifeMax").value_or(0);
    s.life_armor = scalar_num("CUnit", unit_id, "LifeArmor").value_or(0);
    s.shields_max = scalar_num("CUnit", unit_id, "ShieldsMax").value_or(0);
    s.speed = scalar_num("CUnit", unit_id, "Speed").value_or(0);
    s.radius = scalar_num("CUnit", unit_id, "Radius");
    bool primary_weapon = true;
    for (const std::string& wl : array_links("CUnit", unit_id, "WeaponArray"))
        if (!wl.empty()) {
            if (auto d = weapon_damage(wl)) s.weapon_damage.push_back(*d);
            if (primary_weapon) {
                s.weapon_period = scalar_num("CWeapon", wl, "Period").value_or(0);
                s.weapon_range = scalar_num("CWeapon", wl, "Range").value_or(0);
                primary_weapon = false;
            }
        }
    return s;
}

static const char* kMeleeMods[] = {"core",       "liberty", "libertymulti",
                                   "swarm",      "swarmmulti", "void",
                                   "voidmulti"};

void Catalog::load_files(const casc::Storage& store,
                         const std::vector<std::string>& basenames) {
    for (const char* mod : kMeleeMods) {
        for (const std::string& file : basenames) {
            std::string name =
                std::string("mods/") + mod + ".sc2mod/base.sc2data/GameData/" + file;
            try {
                ingest(store.read_by_name(name));
            } catch (const std::exception&) {
            }
        }
    }
}

void Catalog::load_melee(const casc::Storage& store) {
    load_files(store, {"UnitData.xml", "WeaponData.xml", "EffectData.xml",
                       "ActorData.xml", "ModelData.xml", "SoundData.xml"});
}

}
