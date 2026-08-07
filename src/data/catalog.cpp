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
    if (tag.rfind("CValidator", 0) == 0) return "CValidator";
    if (tag.rfind("CMover", 0) == 0) return "CMover";
    if (tag.rfind("CFootprint", 0) == 0) return "CFootprint";
    if (tag.rfind("CButton", 0) == 0) return "CButton";
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
        std::string family = family_of(e.name);
        if (!id) {
            auto& sing = singletons_[family];
            for (const XmlNode& field : e.children) sing.push_back(field);
            continue;
        }
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

const std::vector<core::XmlNode>& Catalog::singleton_fields(const std::string& family) const {
    static const std::vector<core::XmlNode> kEmpty;
    auto it = singletons_.find(family);
    return it == singletons_.end() ? kEmpty : it->second;
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
    std::vector<std::string> slots;
    std::unordered_map<std::string, size_t> byidx;
    long counter = 0;
    for (const XmlNode& c : children) {
        if (c.name != field) continue;
        const std::string* v = c.attr("Link");
        if (!v) v = c.attr("value");
        std::string val = v ? expand_id(*v, id) : std::string{};
        if (const std::string* rm = c.attr("removed"); rm && *rm != "0" && !rm->empty()) val.clear();
        std::string idx;
        if (const std::string* ix = c.attr("index")) {
            idx = *ix;
            char* end = nullptr;
            long n = std::strtol(idx.c_str(), &end, 10);
            if (end != idx.c_str()) counter = n + 1;
        } else {
            idx = std::to_string(counter++);
        }
        auto it = byidx.find(idx);
        if (it != byidx.end()) {
            slots[it->second] = val;
        } else {
            if (!val.empty()) {
                bool dup = false;
                for (const std::string& s : slots) if (s == val) { dup = true; break; }
                if (dup) continue;
            }
            byidx[idx] = slots.size();
            slots.push_back(val);
        }
    }
    std::vector<std::string> out;
    for (const std::string& s : slots) if (!s.empty()) out.push_back(s);
    return out;
}

std::vector<std::string> Catalog::switch_effects(const std::string& effect_id) const {
    std::vector<std::string> out;
    for (const core::XmlNode& n : resolved("CEffect", effect_id))
        if (n.name == "CaseArray")
            if (const std::string* e = n.attr("Effect"); e && !e->empty()) out.push_back(*e);
    if (auto d = scalar("CEffect", effect_id, "DefaultEffect"); d && !d->empty())
        out.push_back(*d);
    return out;
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
        const int pc = int(scalar_num("CEffect", effect_id, "PeriodCount").value_or(1));
        if (auto d = follow_array("PeriodicEffectArray")) return *d * (pc > 0 ? pc : 1);
        if (auto d = follow("InitialEffect")) return d;
    } else if (tag == "CEffectSet") {
        if (auto d = follow_array("EffectArray")) return d;
    } else if (tag == "CEffectSwitch") {
        for (const std::string& ce : switch_effects(effect_id))
            if (auto d = effect_damage(ce, depth + 1)) return d;
    }
    for (const char* f : {"ImpactEffect", "InitialEffect", "FinalEffect", "ExpireEffect"})
        if (auto d = follow(f)) return d;
    for (const char* f : {"EffectArray", "PeriodicEffectArray", "CaseArray"})
        if (auto d = follow_array(f)) return d;
    return std::nullopt;
}

std::optional<double> Catalog::effect_life_change(const std::string& effect_id, int depth) const {
    if (depth > 32) return std::nullopt;
    const Entry* e = find("CEffect", effect_id);
    if (!e) return std::nullopt;
    const std::string& tag = e->tag;

    if (tag == "CEffectModifyUnit") {
        std::optional<double> change;
        for (const core::XmlNode& n : resolved("CEffect", effect_id)) {
            if (n.name != "VitalArray" || n.attr_or("index", "") != "Life") continue;
            for (const core::XmlNode& c : n.children)
                if (c.name == "Change")
                    if (const std::string* v = c.attr("value"); v && !v->empty())
                        change = std::strtod(v->c_str(), nullptr);
        }
        return change;
    }
    auto follow = [&](const std::string& field) -> std::optional<double> {
        if (auto link = scalar("CEffect", effect_id, field); link && !link->empty())
            return effect_life_change(*link, depth + 1);
        return std::nullopt;
    };
    auto follow_array = [&](const std::string& field) -> std::optional<double> {
        for (const std::string& link : array_links("CEffect", effect_id, field))
            if (!link.empty())
                if (auto d = effect_life_change(link, depth + 1)) return d;
        return std::nullopt;
    };
    for (const char* f : {"ImpactEffect", "InitialEffect", "FinalEffect", "ExpireEffect"})
        if (auto d = follow(f)) return d;
    for (const char* f : {"EffectArray", "PeriodicEffectArray", "CaseArray"})
        if (auto d = follow_array(f)) return d;
    return std::nullopt;
}

std::optional<double> Catalog::effect_energy_fraction(const std::string& effect_id, int depth) const {
    if (depth > 32) return std::nullopt;
    const Entry* e = find("CEffect", effect_id);
    if (!e) return std::nullopt;
    const std::string& tag = e->tag;

    if (tag == "CEffectDamage") {
        std::optional<double> frac;
        for (const core::XmlNode& n : resolved("CEffect", effect_id))
            if (n.name == "VitalFractionCurrent" && n.attr_or("index", "") == "Energy")
                if (const std::string* v = n.attr("value"); v && !v->empty())
                    frac = std::strtod(v->c_str(), nullptr);
        return frac;
    }
    auto follow = [&](const std::string& field) -> std::optional<double> {
        if (auto link = scalar("CEffect", effect_id, field); link && !link->empty())
            return effect_energy_fraction(*link, depth + 1);
        return std::nullopt;
    };
    auto follow_array = [&](const std::string& field) -> std::optional<double> {
        for (const std::string& link : array_links("CEffect", effect_id, field))
            if (!link.empty())
                if (auto d = effect_energy_fraction(link, depth + 1)) return d;
        return std::nullopt;
    };
    for (const char* f : {"ImpactEffect", "InitialEffect", "FinalEffect", "ExpireEffect"})
        if (auto d = follow(f)) return d;
    for (const char* f : {"EffectArray", "PeriodicEffectArray", "CaseArray"})
        if (auto d = follow_array(f)) return d;
    return std::nullopt;
}

std::optional<Catalog::BehaviorBuff> Catalog::behavior_buff(const std::string& behavior_id) const {
    const Entry* e = find("CBehavior", behavior_id);
    if (!e || e->tag.rfind("CBehaviorBuff", 0) != 0) return std::nullopt;
    BehaviorBuff bb;
    bool any = false;
    if (auto d = scalar_num("CBehavior", behavior_id, "Duration")) { bb.duration = *d; any = true; }
    if (auto p = scalar_num("CBehavior", behavior_id, "Period")) bb.dmg_period = *p;
    if (auto pe = scalar("CBehavior", behavior_id, "PeriodicEffect"); pe && !pe->empty())
        if (auto d = effect_damage_f(*pe)) { bb.dmg_per_tick = *d; any = true; }
    for (const core::XmlNode& n : resolved("CBehavior", behavior_id)) {
        if (n.name != "Modification") continue;
        any = true;
        if (const std::string* v = n.attr("MoveSpeedMultiplier"); v && !v->empty())
            bb.move_speed_mult = std::strtod(v->c_str(), nullptr);
        if (const std::string* v = n.attr("AttackSpeedMultiplier"); v && !v->empty())
            bb.attack_speed_mult = std::strtod(v->c_str(), nullptr);
        for (const core::XmlNode& g : n.children)
            if (g.name == "StateFlags") {
                const std::string idx = g.attr_or("index", "");
                if (idx == "CanAttack" && g.attr_or("value", "") == "0") bb.root = false;
                if (idx == "Stunned" && g.attr_or("value", "") == "1") bb.stun = true;
            }
    }
    if (bb.move_speed_mult == 0.0) bb.root = true;
    return any ? std::optional<BehaviorBuff>(bb) : std::nullopt;
}

std::optional<std::string> Catalog::effect_apply_behavior(const std::string& effect_id,
                                                          int depth) const {
    if (depth > 32) return std::nullopt;
    const Entry* e = find("CEffect", effect_id);
    if (!e) return std::nullopt;
    if (e->tag == "CEffectApplyBehavior") {
        for (const core::XmlNode& n : resolved("CEffect", effect_id))
            if (n.name == "Behavior")
                if (const std::string* v = n.attr("value"); v && !v->empty()) return *v;
        if (auto b = scalar("CEffect", effect_id, "Behavior"); b && !b->empty()) return *b;
        return std::nullopt;
    }
    auto follow = [&](const std::string& field) -> std::optional<std::string> {
        if (auto link = scalar("CEffect", effect_id, field); link && !link->empty())
            return effect_apply_behavior(*link, depth + 1);
        return std::nullopt;
    };
    auto follow_array = [&](const std::string& field) -> std::optional<std::string> {
        for (const std::string& link : array_links("CEffect", effect_id, field))
            if (!link.empty())
                if (auto d = effect_apply_behavior(link, depth + 1)) return d;
        return std::nullopt;
    };
    for (const char* f : {"ImpactEffect", "InitialEffect", "FinalEffect", "ExpireEffect"})
        if (auto d = follow(f)) return d;
    for (const char* f : {"EffectArray", "PeriodicEffectArray", "CaseArray"})
        if (auto d = follow_array(f)) return d;
    for (const core::XmlNode& n : resolved("CEffect", effect_id))
        if (n.name == "AreaArray")
            if (const std::string* ef = n.attr("Effect"); ef && !ef->empty())
                if (auto d = effect_apply_behavior(*ef, depth + 1)) return d;
    return std::nullopt;
}

std::optional<Catalog::CreateUnitSpec> Catalog::effect_create_unit(const std::string& effect_id,
                                                                  int depth) const {
    if (depth > 32) return std::nullopt;
    const Entry* e = find("CEffect", effect_id);
    if (!e) return std::nullopt;
    if (e->tag == "CEffectCreateUnit") {
        CreateUnitSpec s;
        if (auto u = scalar("CEffect", effect_id, "SpawnUnit"); u && !u->empty()) s.unit = *u;
        if (auto c = scalar("CEffect", effect_id, "SpawnCount"); c && !c->empty())
            s.count = std::atoi(c->c_str());
        if (auto f = scalar("CEffect", effect_id, "SpawnEffect"); f && !f->empty())
            s.spawn_effect = *f;
        if (s.unit.empty()) return std::nullopt;
        return s;
    }
    auto follow = [&](const std::string& field) -> std::optional<CreateUnitSpec> {
        if (auto link = scalar("CEffect", effect_id, field); link && !link->empty())
            return effect_create_unit(*link, depth + 1);
        return std::nullopt;
    };
    auto follow_array = [&](const std::string& field) -> std::optional<CreateUnitSpec> {
        for (const std::string& link : array_links("CEffect", effect_id, field))
            if (!link.empty())
                if (auto d = effect_create_unit(link, depth + 1)) return d;
        return std::nullopt;
    };
    for (const char* f : {"ImpactEffect", "InitialEffect", "FinalEffect", "ExpireEffect"})
        if (auto d = follow(f)) return d;
    for (const char* f : {"EffectArray", "PeriodicEffectArray", "CaseArray"})
        if (auto d = follow_array(f)) return d;
    return std::nullopt;
}

std::optional<std::pair<double, double>> Catalog::area_damage_radius(const std::string& effect_id,
                                                                     int depth) const {
    if (depth > 32) return std::nullopt;
    const Entry* e = find("CEffect", effect_id);
    if (!e) return std::nullopt;
    if (e->tag == "CEffectEnumArea") {
        double radius = 0;
        std::string dmg_eff;
        for (const core::XmlNode& n : resolved("CEffect", effect_id))
            if (n.name == "AreaArray") {
                if (const std::string* r = n.attr("Radius")) radius = std::strtod(r->c_str(), nullptr);
                if (const std::string* ef = n.attr("Effect"); ef && !ef->empty()) dmg_eff = *ef;
            }
        if (dmg_eff.empty()) return std::nullopt;
        std::optional<double> amt = effect_damage_f(dmg_eff);
        if (!amt)
            if (auto beh = effect_apply_behavior(dmg_eff, depth + 1))
                if (auto pe = scalar("CBehavior", *beh, "PeriodicEffect"); pe && !pe->empty())
                    amt = effect_damage_f(*pe);
        if (amt) return std::make_pair(*amt, radius);
        return std::nullopt;
    }
    for (const char* f : {"ImpactEffect", "InitialEffect", "FinalEffect", "ExpireEffect"})
        if (auto link = scalar("CEffect", effect_id, f); link && !link->empty())
            if (auto d = area_damage_radius(*link, depth + 1)) return d;
    for (const char* f : {"EffectArray", "PeriodicEffectArray", "CaseArray"})
        for (const std::string& link : array_links("CEffect", effect_id, f))
            if (!link.empty())
                if (auto d = area_damage_radius(link, depth + 1)) return d;
    return std::nullopt;
}

std::optional<Catalog::PersistentSpec> Catalog::effect_persistent(const std::string& effect_id,
                                                                  int depth) const {
    if (depth > 32) return std::nullopt;
    const Entry* e = find("CEffect", effect_id);
    if (!e) return std::nullopt;
    if (e->tag == "CEffectCreatePersistent") {
        PersistentSpec s;
        s.count = int(scalar_num("CEffect", effect_id, "PeriodCount").value_or(0));
        if (auto pp = array_links("CEffect", effect_id, "PeriodicPeriodArray");
            !pp.empty() && !pp[0].empty())
            s.period = std::strtod(pp[0].c_str(), nullptr);
        else if (auto p = scalar_num("CEffect", effect_id, "Period"))
            s.period = *p;
        for (const std::string& pe : array_links("CEffect", effect_id, "PeriodicEffectArray"))
            if (!pe.empty())
                if (auto ar = area_damage_radius(pe, depth + 1)) {
                    s.amount = ar->first;
                    s.radius = ar->second;
                    break;
                }
        if (s.amount == 0 && s.radius == 0) return std::nullopt;
        return s;
    }
    for (const char* f : {"ImpactEffect", "InitialEffect", "FinalEffect", "ExpireEffect"})
        if (auto link = scalar("CEffect", effect_id, f); link && !link->empty())
            if (auto d = effect_persistent(*link, depth + 1)) return d;
    for (const char* f : {"EffectArray", "CaseArray"})
        for (const std::string& link : array_links("CEffect", effect_id, f))
            if (!link.empty())
                if (auto d = effect_persistent(link, depth + 1)) return d;
    return std::nullopt;
}

std::optional<std::string> Catalog::damage_effect_id(const std::string& effect_id,
                                                     int depth) const {
    if (depth > 32) return std::nullopt;
    const Entry* e = find("CEffect", effect_id);
    if (!e) return std::nullopt;
    const std::string& tag = e->tag;
    if (tag == "CEffectDamage") return effect_id;

    auto follow = [&](const std::string& field) -> std::optional<std::string> {
        if (auto link = scalar("CEffect", effect_id, field); link && !link->empty())
            return damage_effect_id(*link, depth + 1);
        return std::nullopt;
    };
    auto follow_array = [&](const std::string& field) -> std::optional<std::string> {
        for (const std::string& link : array_links("CEffect", effect_id, field))
            if (!link.empty())
                if (auto d = damage_effect_id(link, depth + 1)) return d;
        return std::nullopt;
    };
    if (tag == "CEffectLaunchMissile") {
        if (auto d = follow("ImpactEffect")) return d;
    } else if (tag == "CEffectCreatePersistent") {
        if (auto d = follow_array("PeriodicEffectArray")) return d;
        if (auto d = follow("InitialEffect")) return d;
    } else if (tag == "CEffectSet") {
        if (auto d = follow_array("EffectArray")) return d;
    } else if (tag == "CEffectSwitch") {
        for (const std::string& ce : switch_effects(effect_id))
            if (auto d = damage_effect_id(ce, depth + 1)) return d;
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

std::vector<std::pair<double, double>>
Catalog::weapon_splash(const std::string& weapon_id) const {
    std::vector<std::pair<double, double>> rings;
    const Entry* w = find("CWeapon", weapon_id);
    if (!w) return rings;
    std::string effect = scalar("CWeapon", weapon_id, "Effect").value_or("");
    if (effect.empty()) effect = weapon_id;
    auto did = damage_effect_id(effect, 0);
    if (!did) return rings;
    for (const core::XmlNode& n : resolved("CEffect", *did)) {
        if (n.name != "AreaArray") continue;
        const std::string* r = n.attr("Radius");
        const std::string* f = n.attr("Fraction");
        if (!r) continue;
        double radius = 0, frac = 1.0;
        try { radius = std::stod(*r); } catch (...) { continue; }
        if (f) { try { frac = std::stod(*f); } catch (...) {} }
        rings.emplace_back(radius, frac);
    }
    return rings;
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
    load_files(store, {"UnitData.xml", "WeaponData.xml", "EffectData.xml", "AbilData.xml",
                       "BehaviorData.xml", "UpgradeData.xml", "ValidatorData.xml", "MoverData.xml",
                       "FootprintData.xml", "ButtonData.xml", "RequirementData.xml", "ActorData.xml",
                       "ModelData.xml", "SoundData.xml"});
}

std::optional<Catalog::MorphSpec> Catalog::morph_spec(const std::string& abil_id) const {
    const Entry* e = find("CAbil", abil_id);
    if (!e || e->tag != "CAbilMorph") return std::nullopt;

    struct Section { std::string name; double delay = 0, dur = 0; };
    struct Stage { std::string unit; std::vector<Section> secs; };
    std::vector<Stage> stages;
    std::string finish_effect;
    int autopos = 0;
    auto stage_at = [&](int slot) -> Stage& {
        if (slot < 0) slot = 0;
        if (int(stages.size()) <= slot) stages.resize(slot + 1);
        return stages[slot];
    };
    for (const XmlNode& info : resolved("CAbil", abil_id)) {
        if (info.name != "InfoArray") continue;
        const std::string* idxa = info.attr("index");
        Stage& st = stage_at(idxa ? std::atoi(idxa->c_str()) : autopos++);
        if (const std::string* u = info.attr("Unit"); u && !u->empty()) st.unit = *u;
        for (const XmlNode& sec : info.children) {
            if (sec.name != "SectionArray") continue;
            const std::string& sname = sec.attr_or("index", "");
            for (const XmlNode& ef : sec.children)
                if (ef.name == "EffectArray" && ef.attr_or("index", "") == "Finish")
                    if (const std::string* v = ef.attr("value"); v && !v->empty()) finish_effect = *v;
            if (sname == "Actor") continue;
            Section* slot = nullptr;
            for (Section& s : st.secs)
                if (s.name == sname) { slot = &s; break; }
            if (!slot) { st.secs.push_back({sname, 0, 0}); slot = &st.secs.back(); }
            for (const XmlNode& d : sec.children) {
                if (d.name != "DurationArray") continue;
                const std::string* v = d.attr("value");
                const double val = v ? std::atof(v->c_str()) : 0.0;
                const std::string& idx = d.attr_or("index", "");
                if (idx == "Delay") slot->delay = val;
                else if (idx == "Duration") slot->dur = val;
            }
        }
    }

    MorphSpec spec;
    for (const Stage& st : stages) {
        if (st.unit.empty() || st.unit.find("##") != std::string::npos) continue;
        double stage_time = 0.0;
        for (const Section& s : st.secs)
            if (s.delay + s.dur > stage_time) stage_time = s.delay + s.dur;
        spec.chain.push_back(st.unit);
        spec.morph_time += stage_time;
    }
    if (spec.chain.empty()) return std::nullopt;
    spec.target = spec.chain.back();
    spec.finish_effect = finish_effect;
    return spec;
}

std::optional<Catalog::SpawnBehavior> Catalog::spawn_behavior(const std::string& behavior_id) const {
    const Entry* e = find("CBehavior", behavior_id);
    if (!e || e->tag != "CBehaviorSpawn") return std::nullopt;
    SpawnBehavior sb;
    bool any = false;
    int autopos = 0;
    for (const XmlNode& info : resolved("CBehavior", behavior_id)) {
        if (info.name != "InfoArray") continue;
        const std::string* idxa = info.attr("index");
        const int slot = idxa ? std::atoi(idxa->c_str()) : autopos++;
        if (slot != 0) continue;
        auto field = [&](const char* key) -> const std::string* {
            if (const std::string* a = info.attr(key)) return a;
            for (const XmlNode& c : info.children)
                if (c.name == key) if (const std::string* v = c.attr("value")) return v;
            return nullptr;
        };
        if (const std::string* u = field("Unit"); u && !u->empty()) { sb.unit = *u; any = true; }
        if (const std::string* m = field("MaxCount")) { sb.max_count = std::atoi(m->c_str()); any = true; }
        if (const std::string* d = field("Delay")) { sb.period = std::atof(d->c_str()); any = true; }
    }
    if (!any || sb.unit.empty()) return std::nullopt;
    if (auto lim = scalar_num("CBehavior", behavior_id, "Limit")) sb.hard_cap = int(*lim);
    return sb;
}

std::optional<double> Catalog::effect_vital_fraction(const std::string& effect_id,
                                                     const std::string& vital) const {
    const Entry* e = find("CEffect", effect_id);
    if (!e || e->tag != "CEffectModifyUnit") return std::nullopt;
    for (const XmlNode& va : resolved("CEffect", effect_id)) {
        if (va.name != "VitalArray" || va.attr_or("index", "") != vital) continue;
        for (const XmlNode& c : va.children)
            if (c.name == "ChangeFraction")
                if (const std::string* v = c.attr("value"); v && !v->empty())
                    return std::atof(v->c_str());
    }
    return std::nullopt;
}

}
