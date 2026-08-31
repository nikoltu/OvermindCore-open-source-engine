#include <algorithm>
#include "sc2/data/catalog.hpp"

#include <cstdlib>
#include <functional>

#include "sc2/casc/storage.hpp"
#include "sc2/data/melee_mods.hpp"

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
    if (tag.rfind("C", 0) == 0 &&
        std::find(unlisted_families_.begin(), unlisted_families_.end(), tag) ==
            unlisted_families_.end())
        unlisted_families_.push_back(tag);
    return tag;
}

std::vector<std::string> Catalog::unlisted_families_;

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
            if (const std::string* d = e.attr("default"); d && *d == "1") {
                auto& defs = tag_defaults_[e.name];
                for (const XmlNode& field : e.children)
                    if (!field.attr("index")) defs.push_back(field);
            }
            continue;
        }
        IdMap& fam = families_[family];
        if (fam.find(*id) == fam.end()) family_order_[family].push_back(*id);
        Entry& entry = fam[*id];
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
        const std::uint16_t decl = entry.decl_seen++;
        for (const XmlNode& field : e.children) {
            entry.own.push_back(field);
            entry.own_decl.push_back(decl);
        }
    }
    resolved_cache_.clear();
}

const std::vector<core::XmlNode>& Catalog::singleton_fields(const std::string& family) const {
    static const std::vector<core::XmlNode> kEmpty;
    auto it = singletons_.find(family);
    return it == singletons_.end() ? kEmpty : it->second;
}

std::optional<std::string> Catalog::tag_default_scalar(const std::string& tag,
                                                       const std::string& field) const {
    auto it = tag_defaults_.find(tag);
    if (it == tag_defaults_.end()) return std::nullopt;
    for (auto f = it->second.rbegin(); f != it->second.rend(); ++f) {
        if (f->name != field) continue;
        if (const std::string* v = f->attr("value")) return *v;
        return std::string{};
    }
    return std::nullopt;
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
    auto oit = family_order_.find(family);
    if (oit == family_order_.end()) return {};
    return oit->second;
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
    std::vector<TwoSlot> slots = resolve_array_slots(family, id, field);
    std::vector<std::string> out;
    for (const TwoSlot& s : slots)
        if (s.present && !s.dead && !s.link.empty()) out.push_back(s.link);
    return out;
}

std::vector<Catalog::TwoSlot> Catalog::resolve_array_slots(const std::string& family,
                                                           const std::string& id,
                                                           const std::string& field,
                                                           const std::string& expand_as) const {
    std::vector<TwoSlot> out;
    for (const NodeSlot& s : resolve_node_slots(family, id, field, expand_as)) {
        TwoSlot t;
        t.dead = s.dead;
        for (const SlotWrite& w : s.writes) {
            const std::string* lv = w.node->attr("Link");
            if (!lv) lv = w.node->attr("value");
            if (lv) {
                t.link = expand_id(*lv, w.self);
                t.present = true;
            }
        }
        out.push_back(std::move(t));
    }
    return out;
}

std::vector<Catalog::NodeSlot> Catalog::resolve_node_slots(const std::string& family,
                                                           const std::string& id,
                                                           const std::string& field,
                                                           const std::string& expand_as) const {
    const std::string& self = expand_as.empty() ? id : expand_as;
    const Entry* e = find(family, id);
    if (!e) return {};

    std::vector<NodeSlot> base;
    if (!e->parent.empty() && e->parent != id)
        base = resolve_node_slots(family, e->parent, field, self);

    struct Own { long key; bool is_pos; std::vector<SlotWrite> writes; bool dead; };
    std::vector<Own> own;
    std::unordered_map<long, size_t> own_at;
    long counter = 0;
    long next_append = 0;
    long cur_decl = -1;
    for (size_t k = 0; k < e->own.size(); ++k) {
        if (e->own[k].name != field) {
            if (k < e->own_decl.size() && long(e->own_decl[k]) != cur_decl) {
                cur_decl = long(e->own_decl[k]); counter = next_append;
            }
            continue;
        }
        if (k < e->own_decl.size() && long(e->own_decl[k]) != cur_decl) {
            cur_decl = long(e->own_decl[k]); counter = next_append;
        }
        const XmlNode& c = e->own[k];
        int rm_state = -1;
        if (const std::string* rm = c.attr("removed"))
            rm_state = (*rm != "0" && !rm->empty()) ? 1 : 0;
        bool has_index = false; long index = 0;
        if (const std::string* ix = c.attr("index")) {
            char* end = nullptr; long n = std::strtol(ix->c_str(), &end, 10);
            if (end != ix->c_str() && *end == '\0') { has_index = true; index = n; }
        }
        long key;
        bool is_pos;
        if (has_index) { key = index; is_pos = false; }
        else { key = counter; is_pos = true; ++counter; }
        if (key + 1 > next_append) next_append = key + 1;
        auto it = own_at.find(key);
        if (it == own_at.end()) {
            own_at[key] = own.size();
            own.push_back(Own{key, is_pos, {SlotWrite{&c, self}}, rm_state == 1});
        } else {
            Own& o = own[it->second];
            o.writes.push_back(SlotWrite{&c, self});
            if (rm_state == 1) o.dead = true;
            else if (rm_state == 0) o.dead = false;
        }
    }

    for (Own& o : own) {
        if (o.is_pos) {
            base.push_back(NodeSlot{std::move(o.writes), o.dead});
        } else {
            size_t idx = size_t(o.key < 0 ? 0 : o.key);
            if (idx >= base.size()) base.resize(idx + 1);
            NodeSlot& s = base[idx];
            s.writes.insert(s.writes.end(), o.writes.begin(), o.writes.end());
            if (o.dead) s.dead = true;
        }
    }
    return base;
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

std::optional<int> Catalog::effect_damage(const std::string& effect_id, int depth) const {
    return walk_damage_tree<int>(
        effect_id, depth,
        [this](const std::string& id) -> std::optional<int> {
            if (auto a = scalar_num("CEffect", id, "Amount")) return int(*a);
            return std::nullopt;
        },
        [this](int sub, const std::string& id) -> int {
            const int pc = int(scalar_num("CEffect", id, "PeriodCount").value_or(0));
            return sub * (pc > 0 ? pc : 1);
        });
}

std::optional<double> Catalog::effect_life_change(const std::string& effect_id, int depth) const {
    return walk_effect_first<double>(
        effect_id, depth,
        [this](const std::string& id, const std::string& tag, int, bool& terminal) -> std::optional<double> {
            if (tag != "CEffectModifyUnit") return std::nullopt;
            terminal = true;
            std::optional<double> change;
            for (const core::XmlNode& n : resolved("CEffect", id)) {
                if (n.name != "VitalArray" || n.attr_or("index", "") != "Life") continue;
                for (const core::XmlNode& c : n.children)
                    if (c.name == "Change")
                        if (const std::string* v = c.attr("value"); v && !v->empty())
                            change = std::strtod(v->c_str(), nullptr);
            }
            return change;
        });
}

std::optional<double> Catalog::effect_energy_fraction(const std::string& effect_id, int depth) const {
    return walk_effect_first<double>(
        effect_id, depth,
        [this](const std::string& id, const std::string& tag, int, bool& terminal) -> std::optional<double> {
            if (tag != "CEffectDamage") return std::nullopt;
            terminal = true;
            std::optional<double> frac;
            for (const core::XmlNode& n : resolved("CEffect", id))
                if (n.name == "VitalFractionCurrent" && n.attr_or("index", "") == "Energy")
                    if (const std::string* v = n.attr("value"); v && !v->empty())
                        frac = std::strtod(v->c_str(), nullptr);
            return frac;
        });
}

std::optional<double> Catalog::effect_flat_energy_change(const std::string& effect_id) const {
    return walk_effect_first<double>(
        effect_id, 0,
        [this](const std::string& id, const std::string& tag, int, bool& terminal) -> std::optional<double> {
            if (tag != "CEffectModifyUnit") return std::nullopt;
            terminal = true;
            std::optional<double> change;
            for (const core::XmlNode& n : resolved("CEffect", id)) {
                if (n.name != "VitalArray" || n.attr_or("index", "") != "Energy") continue;
                if (const std::string* v = n.attr("Change"); v && !v->empty())
                    change = std::strtod(v->c_str(), nullptr);
                for (const core::XmlNode& c : n.children)
                    if (c.name == "Change")
                        if (const std::string* v = c.attr("value"); v && !v->empty())
                            change = std::strtod(v->c_str(), nullptr);
            }
            return change;
        });
}

std::optional<Catalog::PointDefense> Catalog::point_defense_of(const std::string& unit_id) const {
    std::function<std::optional<double>(const std::string&, int)> energy_drain =
        [&](const std::string& effect_id, int depth) -> std::optional<double> {
        return walk_effect_first<double>(
            effect_id, depth,
            [this](const std::string& id, const std::string& tag, int,
                   bool& terminal) -> std::optional<double> {
                if (tag != "CEffectModifyUnit") return std::nullopt;
                std::optional<double> change;
                for (const core::XmlNode& n : resolved("CEffect", id)) {
                    if (n.name != "VitalArray" || n.attr_or("index", "") != "Energy") continue;
                    if (const std::string* v = n.attr("Change"); v && !v->empty())
                        change = std::strtod(v->c_str(), nullptr);
                    for (const core::XmlNode& c : n.children)
                        if (c.name == "Change")
                            if (const std::string* v = c.attr("value"); v && !v->empty())
                                change = std::strtod(v->c_str(), nullptr);
                }
                if (change && *change != 0.0) terminal = true;
                return change;
            },
            std::function<std::optional<double>(const std::string&, int)>(
                [&](const std::string& id, int nd) -> std::optional<double> {
                    for (const core::XmlNode& n : resolved("CEffect", id))
                        if (n.name == "AreaArray")
                            if (const std::string* ef = n.attr("Effect"); ef && !ef->empty())
                                if (auto d = energy_drain(*ef, nd + 1)) return d;
                    return std::nullopt;
                }));
    };
    for (const std::string& w : array_links("CUnit", unit_id, "WeaponArray")) {
        if (w.empty()) continue;
        auto tf = scalar("CWeapon", w, "TargetFilters");
        if (!tf) continue;
        std::string req = *tf;
        if (auto sc = req.find(';'); sc != std::string::npos) req.resize(sc);
        if (req.find("Missile") == std::string::npos) continue;
        PointDefense pd;
        pd.radius = scalar_num("CWeapon", w, "Range").value_or(0.0);
        if (auto c = energy_drain(weapon_effect_id(w), 0)) {
            const double cost = std::fabs(*c);
            const double energy = scalar_num("CUnit", unit_id, "EnergyStart").value_or(0.0);
            if (cost > 0.0 && energy > 0.0) pd.max = int(energy / cost);
        }
        if (pd.radius > 0.0 && pd.max > 0) return pd;
    }
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
    for (const core::XmlNode& n : resolved("CBehavior", behavior_id)) {
        if (n.name != "Player") continue;
        const std::string* v = n.attr("Value");
        if (!v) v = n.attr("value");
        if (v && *v == "Caster") { bb.control_transfer = true; any = true; }
    }
    return any ? std::optional<BehaviorBuff>(bb) : std::nullopt;
}

std::optional<std::string> Catalog::effect_control_transfer_behavior(const std::string& effect_id) const {
    return walk_effect_first<std::string>(
        effect_id, 0,
        [this](const std::string& id, const std::string& tag, int,
               bool& terminal) -> std::optional<std::string> {
            if (tag != "CEffectApplyBehavior") return std::nullopt;
            if (auto beh = effect_apply_behavior(id))
                if (auto bb = behavior_buff(*beh); bb && bb->control_transfer && bb->duration > 0.0) {
                    terminal = true;
                    return *beh;
                }
            return std::nullopt;
        });
}

Catalog::AbilityAutocast Catalog::ability_autocast(const std::string& abil_id) const {
    AbilityAutocast a;
    for (const core::XmlNode& n : resolved("CAbil", abil_id)) {
        if (n.name != "Flags") continue;
        const std::string* idx = n.attr("index");
        if (!idx) continue;
        const std::string& v = n.attr_or("value", "1");
        const bool on = !v.empty() && v != "0" && v != "false";
        if (*idx == "AutoCast") a.offered = on;
        else if (*idx == "AutoCastOn") a.on_by_default = on;
    }
    if (auto r = scalar_num("CAbil", abil_id, "AutoCastRange")) a.range = *r;
    else if (auto c = scalar_num("CAbil", abil_id, "Range")) a.range = *c;
    if (auto f = scalar("CAbil", abil_id, "AutoCastFilters"); f && !f->empty()) {
        const std::size_t semi = f->find(';');
        if (semi != std::string::npos && f->find("Enemy", semi) != std::string::npos)
            a.targets_allies = true;
    }
    return a;
}

Catalog::DamageResponse Catalog::behavior_damage_response(const std::string& behavior_id) const {
    DamageResponse r;
    const Entry* e = find("CBehavior", behavior_id);
    if (!e) return r;
    auto field = [](const core::XmlNode& dr, const char* name) -> std::optional<double> {
        if (const std::string* v = dr.attr(name); v && !v->empty())
            return std::strtod(v->c_str(), nullptr);
        for (const core::XmlNode& c : dr.children)
            if (c.name == name)
                if (const std::string* v = c.attr("value"); v && !v->empty())
                    return std::strtod(v->c_str(), nullptr);
        return std::nullopt;
    };
    for (const core::XmlNode& n : resolved("CBehavior", behavior_id)) {
        if (n.name != "DamageResponse") continue;
        r.present = true;
        if (auto v = field(n, "ModifyFraction")) r.fraction = *v;
        if (auto v = field(n, "ModifyLimit")) r.modify_limit = *v;
        if (auto v = field(n, "ClampMaximum")) r.clamp_max = *v;
        if (auto v = field(n, "ClampMinimum")) r.clamp_min = *v;
        if (auto v = field(n, "Minimum")) r.minimum = *v;
        if (auto v = field(n, "ModifyMinimumDamage")) r.minimum = *v;
        if (auto v = field(n, "ModifyAmount")) r.modify_amount = *v;
        for (const core::XmlNode& cost : n.children) {
            if (cost.name != "Cost") continue;
            for (const core::XmlNode& cd : cost.children) {
                if (cd.name != "Cooldown") continue;
                if (auto v = field(cd, "TimeUse")) r.cooldown = *v;
            }
        }
        for (const core::XmlNode& k : n.children)
            if (k.name == "Kind" && k.attr_or("index", "") == "Spell" && k.attr_or("value", "") == "0")
                r.excludes_spell = true;
    }
    return r;
}

std::optional<std::string> Catalog::effect_apply_behavior(const std::string& effect_id,
                                                          int depth) const {
    return walk_effect_first<std::string>(
        effect_id, depth,
        [this](const std::string& id, const std::string& tag, int,
               bool& terminal) -> std::optional<std::string> {
            if (tag != "CEffectApplyBehavior") return std::nullopt;
            terminal = true;
            for (const core::XmlNode& n : resolved("CEffect", id))
                if (n.name == "Behavior")
                    if (const std::string* v = n.attr("value"); v && !v->empty()) return *v;
            if (auto b = scalar("CEffect", id, "Behavior"); b && !b->empty()) return *b;
            if (find("CBehavior", id)) return id;
            return std::nullopt;
        },
        [this](const std::string& id, int node_depth) -> std::optional<std::string> {
            for (const core::XmlNode& n : resolved("CEffect", id))
                if (n.name == "AreaArray")
                    if (const std::string* ef = n.attr("Effect"); ef && !ef->empty())
                        if (auto d = effect_apply_behavior(*ef, node_depth + 1)) return d;
            return std::nullopt;
        });
}

std::optional<std::string> Catalog::effect_movement_slow_behavior(const std::string& effect_id) const {
    return walk_effect_first<std::string>(
        effect_id, 0,
        [this](const std::string& id, const std::string& tag, int,
               bool& terminal) -> std::optional<std::string> {
            if (tag != "CEffectSet") return std::nullopt;
            std::vector<std::string> behs;
            bool any = false;
            for (const std::string& c : effect_set_children(id)) {
                if (c.empty()) continue;
                any = true;
                if (effect_subclass(c) != "ApplyBehavior") return std::nullopt;
                if (auto b = effect_apply_behavior(c)) behs.push_back(*b);
            }
            if (!any) return std::nullopt;
            terminal = true;
            for (const std::string& b : behs)
                if (auto bb = behavior_buff(b); bb && bb->move_speed_mult < 1.0 && bb->move_speed_mult >= 0.0)
                    return b;
            return std::nullopt;
        },
        [this](const std::string& id, int) -> std::optional<std::string> {
            for (const core::XmlNode& n : resolved("CEffect", id))
                if (n.name == "AreaArray")
                    if (const std::string* ef = n.attr("Effect"); ef && !ef->empty())
                        if (auto d = effect_movement_slow_behavior(*ef)) return d;
            return std::nullopt;
        });
}

std::optional<Catalog::CreateUnitSpec> Catalog::effect_create_unit(const std::string& effect_id,
                                                                  int depth) const {
    return walk_effect_first<CreateUnitSpec>(
        effect_id, depth,
        [this](const std::string& id, const std::string& tag, int,
               bool& terminal) -> std::optional<CreateUnitSpec> {
            if (tag != "CEffectCreateUnit") return std::nullopt;
            terminal = true;
            CreateUnitSpec s;
            if (auto u = scalar("CEffect", id, "SpawnUnit"); u && !u->empty()) s.unit = *u;
            if (auto c = scalar("CEffect", id, "SpawnCount"); c && !c->empty())
                s.count = std::atoi(c->c_str());
            if (auto f = scalar("CEffect", id, "SpawnEffect"); f && !f->empty()) s.spawn_effect = *f;
            if (s.unit.empty()) return std::nullopt;
            return s;
        });
}

std::optional<double> Catalog::apply_behavior_duration(const std::string& effect_id, int depth) const {
    return walk_effect_first<double>(
        effect_id, depth,
        [this](const std::string& id, const std::string& tag, int,
               bool& terminal) -> std::optional<double> {
            if (tag != "CEffectApplyBehavior") return std::nullopt;
            if (auto d = scalar_num("CEffect", id, "Duration"); d && *d > 0) {
                terminal = true;
                return *d;
            }
            return std::nullopt;
        });
}

std::optional<Catalog::AttackSpawnSpec> Catalog::attack_spawn_of(const std::string& unit_id) const {
    std::string mag;
    for (const std::string& a : array_links("CUnit", unit_id, "AbilArray")) {
        const Entry* ae = a.empty() ? nullptr : find("CAbil", a);
        if (ae && ae->tag == "CAbilArmMagazine") { mag = a; break; }
    }
    if (mag.empty()) return std::nullopt;
    std::string escort;
    int count = 0;
    for (const XmlNode& info : resolved("CAbil", mag)) {
        if (info.name != "InfoArray") continue;
        if (const std::string* u = info.attr("Unit"); u && !u->empty()) escort = *u;
        if (const std::string* c = info.attr("CountStart"); c && !c->empty()) count = std::atoi(c->c_str());
        if (!escort.empty()) break;
    }
    if (escort.empty() || count <= 0) return std::nullopt;
    const auto wl = array_links("CUnit", escort, "WeaponArray");
    if (wl.empty() || wl[0].empty()) return std::nullopt;
    std::string spawn_unit, spawn_effect;
    std::function<bool(const std::string&, int)> find_cu = [&](const std::string& eid, int d) -> bool {
        if (d > 32 || eid.empty()) return false;
        const Entry* e = find("CEffect", eid);
        if (!e) return false;
        if (e->tag == "CEffectCreateUnit") {
            if (auto u = scalar("CEffect", eid, "SpawnUnit"); u && !u->empty()) {
                spawn_unit = *u;
                if (auto f = scalar("CEffect", eid, "SpawnEffect"); f && !f->empty()) spawn_effect = *f;
                return true;
            }
        }
        for (const char* f :
             {"ImpactEffect", "FinishEffect", "InitialEffect", "FinalEffect", "ExpireEffect", "SpawnEffect"})
            if (auto l = scalar("CEffect", eid, f); l && !l->empty())
                if (find_cu(*l, d + 1)) return true;
        for (const XmlNode& n : resolved("CEffect", eid)) {
            if (n.name != "EffectArray" && n.name != "CaseArray" && n.name != "CaseDefault") continue;
            const std::string* v = n.attr("Effect");
            if (!v) v = n.attr("Link");
            if (!v) v = n.attr("value");
            if (v && !v->empty() && find_cu(*v, d + 1)) return true;
        }
        return false;
    };
    if (!find_cu(weapon_effect_id(wl[0]), 0) || spawn_unit.empty()) return std::nullopt;
    AttackSpawnSpec s;
    s.unit = spawn_unit;
    s.count = count;
    if (!spawn_effect.empty())
        if (auto dur = apply_behavior_duration(spawn_effect, 0)) s.life = *dur;
    return s;
}

std::optional<Catalog::InterceptorMagSpec> Catalog::interceptor_magazine_of(
    const std::string& unit_id) const {
    std::string mag;
    for (const std::string& a : array_links("CUnit", unit_id, "AbilArray")) {
        const Entry* ae = a.empty() ? nullptr : find("CAbil", a);
        if (ae && ae->tag == "CAbilArmMagazine") { mag = a; break; }
    }
    if (mag.empty()) return std::nullopt;
    const auto& nodes = resolved("CAbil", mag);
    std::string slot;
    for (const XmlNode& info : nodes) {
        if (info.name != "InfoArray") continue;
        const std::string* u = info.attr("Unit");
        if (u && !u->empty()) { slot = info.attr_or("index", ""); break; }
    }
    if (slot.empty()) return std::nullopt;
    std::string escort;
    int count = 0;
    double build_time = 0;
    for (const XmlNode& info : nodes) {
        if (info.name != "InfoArray" || info.attr_or("index", "") != slot) continue;
        if (const std::string* u = info.attr("Unit"); u && !u->empty()) escort = *u;
        if (const std::string* c = info.attr("CountStart"); c && !c->empty()) count = std::atoi(c->c_str());
        if (const std::string* t = info.attr("Time"); t && !t->empty())
            build_time = std::strtod(t->c_str(), nullptr);
    }
    if (escort.empty()) return std::nullopt;
    InterceptorMagSpec s;
    s.unit = escort;
    s.count = count;
    s.count_max = int(scalar_num("CAbil", mag, "MaxCount").value_or(count));
    if (auto l = scalar_num("CAbil", mag, "Leash")) s.leash = *l;
    s.build_time = build_time;
    return s;
}

std::optional<Catalog::NovaLaunchSpec> Catalog::nova_launch_of(const std::string& unit_id) const {
    for (const std::string& ab : array_links("CUnit", unit_id, "AbilArray")) {
        const Entry* ae = ab.empty() ? nullptr : find("CAbil", ab);
        if (!ae || ae->tag != "CAbilEffectTarget") continue;
        std::string eff;
        for (const XmlNode& n : resolved("CAbil", ab))
            if (n.name == "Effect")
                if (const std::string* v = n.attr("value"); v && !v->empty()) eff = *v;
        if (eff.empty()) continue;
        const auto cu = effect_create_unit(eff);
        if (!cu || cu->unit.empty() || cu->spawn_effect.empty()) continue;
        const double base_speed = scalar_num("CUnit", cu->unit, "Speed").value_or(0.0);
        if (base_speed <= 0) continue;
        const auto beh = effect_apply_behavior(cu->spawn_effect);
        if (!beh) continue;
        const double dur = scalar_num("CBehavior", *beh, "Duration").value_or(0.0);
        if (dur <= 0) continue;
        const auto expire = scalar("CBehavior", *beh, "ExpireEffect");
        if (!expire || expire->empty()) continue;
        const auto ar = area_damage_radius(*expire, 0);
        if (!ar) continue;
        double speed_bonus = 0.0;
        for (const XmlNode& n : resolved("CBehavior", *beh))
            if (n.name == "Modification")
                if (const std::string* v = n.attr("MoveSpeedBonus"); v && !v->empty())
                    speed_bonus = std::strtod(v->c_str(), nullptr);
        double shield_bonus = 0.0;
        std::function<std::string(const std::string&, int)> area_dmg = [&](const std::string& eid,
                                                                           int d) -> std::string {
            if (d > 32 || eid.empty()) return "";
            for (const XmlNode& n : resolved("CEffect", eid))
                if (n.name == "AreaArray")
                    if (const std::string* ef = n.attr("Effect"); ef && !ef->empty()) return *ef;
            for (const XmlNode& n : resolved("CEffect", eid)) {
                if (n.name != "EffectArray") continue;
                const std::string* v = n.attr("Effect");
                if (!v) v = n.attr("value");
                if (v && !v->empty())
                    if (std::string r = area_dmg(*v, d + 1); !r.empty()) return r;
            }
            return "";
        };
        if (std::string dmg_root = area_dmg(*expire, 0); !dmg_root.empty())
            if (const auto did = damage_effect_id(dmg_root, 0))
                shield_bonus = scalar_num("CEffect", *did, "ShieldBonus").value_or(0.0);
        NovaLaunchSpec s;
        s.ability = ab;
        s.unit = cu->unit;
        s.speed = base_speed + speed_bonus;
        s.damage = ar->first;
        s.radius = ar->second;
        s.shield_bonus = shield_bonus;
        s.duration = dur;
        return s;
    }
    return std::nullopt;
}

std::optional<std::pair<double, double>> Catalog::area_damage_radius(const std::string& effect_id,
                                                                     int depth) const {
    return walk_effect_first<std::pair<double, double>>(
        effect_id, depth,
        [this](const std::string& id, const std::string& tag, int node_depth,
               bool& terminal) -> std::optional<std::pair<double, double>> {
            if (tag != "CEffectEnumArea") return std::nullopt;
            terminal = true;
            double radius = 0;
            std::string dmg_eff;
            for (const core::XmlNode& n : resolved("CEffect", id))
                if (n.name == "AreaArray") {
                    if (const std::string* r = n.attr("Radius")) radius = std::strtod(r->c_str(), nullptr);
                    if (const std::string* ef = n.attr("Effect"); ef && !ef->empty()) dmg_eff = *ef;
                }
            if (dmg_eff.empty()) return std::nullopt;
            std::optional<double> amt = effect_damage_f(dmg_eff);
            if (!amt)
                if (auto beh = effect_apply_behavior(dmg_eff, node_depth + 1))
                    if (auto pe = scalar("CBehavior", *beh, "PeriodicEffect"); pe && !pe->empty())
                        amt = effect_damage_f(*pe);
            if (amt) return std::make_pair(*amt, radius);
            return std::nullopt;
        });
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
        if (s.amount == 0 && s.radius == 0)
            for (const XmlNode& n : resolved("CEffect", effect_id)) {
                if (n.name != "PeriodicEffectArray") continue;
                const std::string* pe = n.attr("value");
                if (!pe) pe = n.attr("Link");
                if (!pe || pe->empty()) continue;
                if (auto ar = area_damage_radius(*pe, depth + 1)) {
                    s.amount = ar->first;
                    s.radius = ar->second;
                    break;
                }
            }
        std::vector<double> offs;
        for (const std::string& off : array_links("CEffect", effect_id, "PeriodicOffsetArray")) {
            if (off.empty()) continue;
            char* end = nullptr;
            const double x = std::strtod(off.c_str(), &end);
            double y = 0.0;
            if (end && *end == ',') y = std::strtod(end + 1, nullptr);
            offs.push_back(x != 0.0 ? x : y);
        }
        if (offs.size() >= 2) {
            s.line_count = static_cast<int>(offs.size());
            s.line_first = *std::min_element(offs.begin(), offs.end());
            s.line_last = *std::max_element(offs.begin(), offs.end());
            s.line_step = (s.line_last - s.line_first) / (s.line_count - 1);
            for (const XmlNode& n : resolved("CEffect", effect_id))
                if (n.name == "WhichLocation")
                    if (const std::string* v = n.attr("Value"); v && *v == "SourcePoint")
                        s.line_from_source = true;
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

std::optional<std::string> Catalog::damage_effect_id(const std::string& effect_id, int depth) const {
    return walk_damage_tree<std::string>(
        effect_id, depth,
        [](const std::string& id) -> std::optional<std::string> { return id; },
        [](std::string sub, const std::string&) -> std::string { return sub; });
}

std::optional<double> Catalog::weapon_radius_split_no_splash(const std::string& weapon_id) const {
    const std::string eff = weapon_effect_id(weapon_id);
    const Entry* e = find("CEffect", eff);
    if (!e || e->tag != "CEffectSwitch") return std::nullopt;
    std::string large_eff, small_eff;
    double threshold = 0.0;
    for (const XmlNode& n : resolved("CEffect", eff)) {
        if (n.name != "CaseArray") continue;
        const std::string val = n.attr_or("Validator", "");
        const std::string ce = n.attr_or("Effect", "");
        if (val.empty() || ce.empty()) continue;
        const Entry* ve = find("CValidator", val);
        if (!ve || ve->tag != "CValidatorUnitCompareField") continue;
        const auto field = scalar("CValidator", val, "Field");
        const auto cmp = scalar("CValidator", val, "Compare");
        const auto value = scalar_num("CValidator", val, "Value");
        if (!field || *field != "Radius" || !cmp) continue;
        if (*cmp == "GE" && value) { large_eff = ce; threshold = *value; }
        else if (*cmp == "LT") { small_eff = ce; }
    }
    if (large_eff.empty() || threshold <= 0.0) return std::nullopt;
    auto rings = [&](const std::string& id) {
        int r = 0;
        for (const XmlNode& n : resolved("CEffect", id))
            if (n.name == "AreaArray") ++r;
        return r;
    };
    const int large_r = rings(large_eff);
    const int small_r = small_eff.empty() ? 1 : rings(small_eff);
    return large_r < small_r ? std::optional<double>(threshold) : std::nullopt;
}

std::optional<int> Catalog::weapon_damage(const std::string& weapon_id) const {
    const Entry* w = find("CWeapon", weapon_id);
    if (!w) return std::nullopt;
    const int hits = weapon_hit_count(weapon_id).value_or(1);
    if (auto leaf = weapon_damage_effect_id(weapon_id))
        if (auto amount = scalar_num("CEffect", *leaf, "Amount")) return int(*amount) * hits;
    return std::nullopt;
}

std::optional<int> Catalog::weapon_hit_count(const std::string& weapon_id) const {
    if (!find("CWeapon", weapon_id)) return std::nullopt;
    if (auto d = scalar_num("CWeapon", weapon_id, "DisplayAttackCount"); d && *d > 0) return int(*d);
    return 1;
}

std::vector<std::pair<double, double>>
Catalog::weapon_splash(const std::string& weapon_id) const {
    std::vector<std::pair<double, double>> rings;
    const Entry* w = find("CWeapon", weapon_id);
    if (!w) return rings;
    auto did = damage_effect_id(weapon_effect_id(weapon_id), 0);
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

std::vector<Catalog::ArcSplash>
Catalog::weapon_arc_splash(const std::string& weapon_id) const {
    std::vector<ArcSplash> arcs;
    const Entry* w = find("CWeapon", weapon_id);
    if (!w) return arcs;
    auto did = damage_effect_id(weapon_effect_id(weapon_id), 0);
    if (!did) return arcs;
    for (const core::XmlNode& n : resolved("CEffect", *did)) {
        if (n.name != "AreaArray") continue;
        const std::string* a = n.attr("Arc");
        const std::string* r = n.attr("Radius");
        if (!a || !r) continue;
        ArcSplash e;
        e.frac = 1.0;
        try { e.arc_deg = std::stod(*a); e.radius = std::stod(*r); } catch (...) { continue; }
        if (const std::string* f = n.attr("Fraction")) { try { e.frac = std::stod(*f); } catch (...) {} }
        arcs.push_back(e);
    }
    return arcs;
}

std::optional<double> Catalog::missile_speed(const std::string& weapon_id) const {
    const std::string eff = weapon_effect_id(weapon_id);
    if (eff.empty()) return std::nullopt;
    const Entry* ee = find("CEffect", eff);
    if (!ee || ee->tag != "CEffectLaunchMissile") return std::nullopt;
    std::string mover;
    if (auto mv = scalar("CEffect", eff, "Movers"); mv && !mv->empty())
        mover = *mv;
    else if (auto au = scalar("CEffect", eff, "AmmoUnit"); au && !au->empty()) {
        if (auto m = scalar("CUnit", *au, "Mover"); m && !m->empty()) mover = *m;
    }
    if (mover.empty()) return std::nullopt;
    for (const core::XmlNode& n : resolved("CMover", mover)) {
        if (n.name != "MotionPhases") continue;
        for (const char* fld : {"MaxSpeed", "Speed"})
            for (const core::XmlNode& c : n.children)
                if (c.name == fld)
                    if (const std::string* v = c.attr("value"); v && !v->empty()) {
                        try { return std::stod(*v); } catch (...) { return std::nullopt; }
                    }
    }
    return std::nullopt;
}

std::string Catalog::ability_command_requirement(const std::string& abil_id,
                                                 const std::string& cmd_index) const {
    std::string req;
    for (const core::XmlNode& n : resolved("CAbil", abil_id)) {
        if (n.name != "CmdButtonArray") continue;
        if (n.attr_or("index", "") != cmd_index) continue;
        const std::string r = n.attr_or("Requirements", "");
        if (!r.empty()) req = r;
    }
    return req;
}

std::vector<Catalog::AbilInfoRow> Catalog::ability_info_rows(const std::string& abil_id) const {
    const Entry* e = find("CAbil", abil_id);
    if (!e) return {};
    const bool is_merge = e->tag == "CAbilMerge";
    const bool is_warp = e->tag == "CAbilWarpTrain";
    std::vector<AbilInfoRow> out;
    std::unordered_map<std::string, std::size_t> at;
    for (const core::XmlNode& info : resolved("CAbil", abil_id)) {
        if (info.name != "InfoArray" && !(is_merge && info.name == "Info")) continue;
        const std::string index = info.attr_or("index", "");
        auto it = at.find(index);
        if (it == at.end()) {
            at[index] = out.size();
            out.push_back(AbilInfoRow{});
            out.back().index = index;
            it = at.find(index);
        }
        AbilInfoRow& r = out[it->second];

        if (const std::string* u = info.attr("Unit")) {
            r.unit = *u;
            r.unit_written = true;
            r.unit_count = 1;
        } else {
            int n = 0;
            std::string first;
            bool any = false;
            for (const core::XmlNode& ch : info.children) {
                if (ch.name != "Unit") continue;
                any = true;
                const std::string v = ch.attr_or("value", "");
                if (v.empty()) continue;
                if (first.empty()) first = v;
                ++n;
            }
            if (any) {
                r.unit = first;
                r.unit_written = true;
                r.unit_count = n > 1 ? n : 1;
            }
        }
        if (const std::string* u = info.attr("Upgrade")) {
            r.upgrade = *u;
            r.upgrade_written = true;
            if (!u->empty()) r.upgrade_named = true;
        } else {
            for (const core::XmlNode& ch : info.children) {
                if (ch.name != "Upgrade") continue;
                r.upgrade = ch.attr_or("value", "");
                r.upgrade_written = true;
                if (!r.upgrade.empty()) r.upgrade_named = true;
                break;
            }
        }
        if (const std::string t = info.attr_or("Time", ""); !t.empty()) r.time = authored_seconds(t.c_str());
        for (const core::XmlNode& ch : info.children) {
            if (ch.name == "Time") {
                if (const std::string* v = ch.attr("value")) r.time = authored_seconds(v->c_str());
            } else if (ch.name == "Resource") {
                const std::string idx = ch.attr_or("index", "");
                const std::string val = ch.attr_or("value", "");
                if (idx == "Minerals") r.minerals = std::atoi(val.c_str());
                else if (idx == "Vespene") r.gas = std::atoi(val.c_str());
            } else if (ch.name == "ValidatorArray") {
                if (ch.attr_or("value", "").find("Vespene") != std::string::npos) r.is_gas = true;
            } else if (ch.name == "Button") {
                if (const std::string* req = ch.attr("Requirements")) {
                    r.requirement = *req;
                    r.requirement_written = true;
                    r.requires_techlab = req->find("Attached") != std::string::npos &&
                                         req->find("TechLab") != std::string::npos;
                }
            } else if (is_warp && ch.name == "Charge") {
                std::string v = ch.attr_or("TimeUse", "");
                if (v.empty())
                    for (const core::XmlNode& tu : ch.children)
                        if (tu.name == "TimeUse") { v = tu.text.empty() ? tu.attr_or("value", "") : tu.text; break; }
                if (!v.empty()) r.warp_charge = std::strtod(v.c_str(), nullptr);
            }
        }
        r.self_macro = r.unit.find("##") != std::string::npos;
    }
    return out;
}

std::optional<Catalog::AbilInfoRow> Catalog::ability_info_row(const std::string& abil_id,
                                                              const std::string& index) const {
    for (AbilInfoRow& r : ability_info_rows(abil_id))
        if (r.index == index) return r;
    return std::nullopt;
}

std::vector<Catalog::CommandCardLayout>
Catalog::unit_command_cards(const std::string& unit_id) const {
    auto parse_int = [](const std::string& s) -> int {
        if (s.empty()) return -1;
        char* end = nullptr;
        long v = std::strtol(s.c_str(), &end, 10);
        return (end != s.c_str() && *end == '\0') ? static_cast<int>(v) : -1;
    };
    auto merge_attr = [](const core::XmlNode& n, const char* key, std::string& dst) {
        if (const std::string* v = n.attr(key)) dst = *v;
    };

    std::vector<CommandCardLayout> out;
    const std::vector<NodeSlot> cards = resolve_node_slots("CUnit", unit_id, "CardLayouts");
    for (std::size_t ci = 0; ci < cards.size(); ++ci) {
        const NodeSlot& card = cards[ci];
        if (card.dead) continue;
        CommandCardLayout layout;
        layout.index = int(ci);
        struct Slot {
            std::vector<const core::XmlNode*> writes;
            bool dead = false;
        };
        std::vector<Slot> slots;
        long next_append = 0;
        for (const SlotWrite& w : card.writes) {
            merge_attr(*w.node, "CardId", layout.card_id);
            long counter = next_append;
            for (const core::XmlNode& b : w.node->children) {
                if (b.name != "LayoutButtons") continue;
                long key;
                bool has_index = false;
                long index = 0;
                if (const std::string* ix = b.attr("index")) {
                    char* end = nullptr;
                    long n = std::strtol(ix->c_str(), &end, 10);
                    if (end != ix->c_str() && *end == '\0') { has_index = true; index = n; }
                }
                if (has_index) key = index;
                else key = counter++;
                if (key < 0) key = 0;
                if (key + 1 > next_append) next_append = key + 1;
                if (std::size_t(key) >= slots.size()) slots.resize(std::size_t(key) + 1);
                Slot& s = slots[std::size_t(key)];
                s.writes.push_back(&b);
                if (const std::string* rm = b.attr("removed"))
                    s.dead = (*rm != "0" && !rm->empty());
            }
        }
        for (std::size_t si = 0; si < slots.size(); ++si) {
            if (slots[si].dead || slots[si].writes.empty()) continue;
            CommandCardSlot cs;
            cs.index = int(si);
            std::string abil_cmd;
            std::string row, col;
            for (const core::XmlNode* b : slots[si].writes) {
                merge_attr(*b, "Type", cs.type);
                merge_attr(*b, "AbilCmd", abil_cmd);
                merge_attr(*b, "Face", cs.face);
                merge_attr(*b, "SubmenuCardId", cs.submenu_id);
                merge_attr(*b, "Requirements", cs.requirements);
                merge_attr(*b, "Row", row);
                merge_attr(*b, "Column", col);
            }
            cs.row = parse_int(row);
            cs.col = parse_int(col);
            if (!abil_cmd.empty()) {
                const std::size_t comma = abil_cmd.find(',');
                cs.ability = comma == std::string::npos ? abil_cmd : abil_cmd.substr(0, comma);
                cs.command = comma == std::string::npos ? std::string() : abil_cmd.substr(comma + 1);
            }
            layout.slots.push_back(std::move(cs));
        }
        out.push_back(std::move(layout));
    }
    return out;
}

std::vector<std::pair<std::string, std::string>>
Catalog::unit_ability_commands(const std::string& unit_id) const {
    std::vector<std::pair<std::string, std::string>> out;
    for (const CommandCardLayout& card : unit_command_cards(unit_id))
        for (const CommandCardSlot& s : card.slots) {
            if (s.type != "AbilCmd" || s.ability.empty()) continue;
            bool dup = false;
            for (const auto& p : out)
                if (p.first == s.ability && p.second == s.command) { dup = true; break; }
            if (!dup) out.emplace_back(s.ability, s.command);
        }
    return out;
}

std::string Catalog::cabil_effect_link(const std::string& abil_id) const {
    std::string eff;
    for (const core::XmlNode& c : resolved("CAbil", abil_id))
        if (c.name == "Effect") {
            const std::string v = c.attr_or("value", "");
            if (!v.empty()) eff = v;
        }
    if (auto s = scalar("CAbil", abil_id, "Effect"); s && !s->empty()) eff = *s;
    if (!eff.empty()) return eff;

    const Entry* e = find("CAbil", abil_id);
    if (!e || (e->tag != "CAbilEffectTarget" && e->tag != "CAbilEffectInstant")) return {};
    const auto def = tag_default_scalar("CAbilEffect", "Effect");
    if (!def || def->empty()) return {};
    const std::string link = expand_id(*def, abil_id);
    return (!link.empty() && find("CEffect", link)) ? link : std::string{};
}

std::optional<std::string> Catalog::button_icon(const std::string& button_id) const {
    std::optional<std::string> icon;
    for (const core::XmlNode& n : resolved("CButton", button_id))
        if (n.name == "Icon") {
            const std::string v = n.attr_or("value", "");
            if (!v.empty()) icon = v;
        }
    return icon;
}

std::vector<Catalog::AbilityCardButton>
Catalog::unit_ability_card_buttons(const std::string& unit_id) const {
    std::vector<AbilityCardButton> out;
    for (const CommandCardLayout& card : unit_command_cards(unit_id))
        for (const CommandCardSlot& s : card.slots) {
            if (s.type != "AbilCmd" || s.ability.empty()) continue;
            bool dup = false;
            for (const AbilityCardButton& e : out)
                if (e.ability == s.ability && e.command == s.command) { dup = true; break; }
            if (dup) continue;
            AbilityCardButton e;
            e.ability = s.ability;
            e.command = s.command;
            e.face = s.face;
            e.row = s.row;
            e.col = s.col;
            e.card = card.index;
            out.push_back(std::move(e));
        }
    return out;
}

std::map<std::string, int>
Catalog::weapon_attribute_bonus(const std::string& weapon_id) const {
    std::map<std::string, int> out;
    auto did = weapon_damage_effect_id(weapon_id);
    if (!did) return out;
    for (const core::XmlNode& n : resolved("CEffect", *did)) {
        if (n.name != "AttributeBonus") continue;
        const std::string* idx = n.attr("index");
        const std::string* val = n.attr("value");
        if (!idx || !val) continue;
        try {
            out[*idx] = std::stoi(*val);
        } catch (...) {
        }
    }
    return out;
}

Catalog::UnitStats Catalog::unit_stats(const std::string& unit_id) const {
    UnitStats s;
    s.life_max = scalar_num("CUnit", unit_id, "LifeMax").value_or(0);
    s.life_armor = scalar_num("CUnit", unit_id, "LifeArmor").value_or(0);
    s.shields_max = scalar_num("CUnit", unit_id, "ShieldsMax").value_or(0);
    s.speed = scalar_num("CUnit", unit_id, "Speed").value_or(0);
    s.radius = scalar_num("CUnit", unit_id, "Radius");
    if (!s.radius && find("CUnit", unit_id)) s.radius = 0.5;
    if (s.radius) s.radius = unit_body_radius(unit_id, *s.radius);
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

void Catalog::load_files(const casc::Storage& store,
                         const std::vector<std::string>& basenames) {
    load_skips_.clear();
    for (const char* mod : kMeleeMods) {
        for (const std::string& file : basenames) {
            std::string name =
                std::string("mods/") + mod + ".sc2mod/base.sc2data/GameData/" + file;
            try {
                ingest(store.read_by_name(name));
            } catch (const std::exception&) {
                load_skips_.push_back(name);
            }
        }
    }
}

void Catalog::load_melee(const casc::Storage& store) {
    load_files(store, {"UnitData.xml", "WeaponData.xml", "EffectData.xml", "AbilData.xml",
                       "BehaviorData.xml", "UpgradeData.xml", "ValidatorData.xml", "MoverData.xml",
                       "FootprintData.xml", "ButtonData.xml", "RequirementData.xml",
                       "RequirementNodeData.xml", "ActorData.xml",
                       "ModelData.xml", "SoundData.xml"});
    load_strings(store);
}

static const char* const kCampaignFiles[] = {
    "UnitData.xml",     "WeaponData.xml",   "EffectData.xml",  "AbilData.xml",
    "BehaviorData.xml", "UpgradeData.xml",  "ValidatorData.xml", "MoverData.xml",
    "FootprintData.xml", "ButtonData.xml",  "RequirementData.xml", "RequirementNodeData.xml",
    "ActorData.xml",    "ModelData.xml",    "SoundData.xml"};

void Catalog::load_root_files(const casc::Storage& store,
                              const std::vector<std::string>& roots,
                              const std::vector<std::string>& basenames) {
    for (const std::string& root : roots) {
        for (const std::string& file : basenames) {
            std::string name = root + "/GameData/" + file;
            try {
                ingest(store.read_by_name(name));
            } catch (const std::exception&) {
                load_skips_.push_back(name);
            }
        }
    }
}

void Catalog::load_campaign(const casc::Storage& store,
                            const std::vector<std::string>& campaign_roots,
                            const std::string& mission_local_root) {
    load_melee(store);

    std::vector<std::string> basenames(std::begin(kCampaignFiles), std::end(kCampaignFiles));
    std::vector<std::string> roots = campaign_roots;
    if (!mission_local_root.empty()) roots.push_back(mission_local_root);
    load_root_files(store, roots, basenames);

    for (const std::string& root : roots) {
        std::string mod_base = root;
        const std::string kData = ".sc2data";
        std::size_t slash = mod_base.rfind('/');
        if (slash != std::string::npos && mod_base.find(kData, slash) != std::string::npos)
            mod_base = mod_base.substr(0, slash);
        std::string name = mod_base + "/enus.sc2data/LocalizedData/GameStrings.txt";
        try {
            strings_.load(store.read_by_name(name));
        } catch (const std::exception&) {
        }
    }
}

void Catalog::load_strings(const casc::Storage& store) {
    for (const char* mod : kMeleeMods) {
        std::string name = std::string("mods/") + mod +
                           ".sc2mod/enus.sc2data/LocalizedData/GameStrings.txt";
        try {
            strings_.load(store.read_by_name(name));
        } catch (const std::exception&) {
        }
    }
}

std::optional<std::string> Catalog::button_display_name(const std::string& button_id) const {
    if (button_id.empty()) return std::nullopt;
    if (const std::string* v = strings_.find("Button/Name/" + button_id)) return *v;
    return std::nullopt;
}

std::optional<std::string> Catalog::unit_display_name(const std::string& unit_id) const {
    if (unit_id.empty()) return std::nullopt;
    if (const std::string* v = strings_.find("Unit/Name/" + unit_id)) return *v;
    return std::nullopt;
}

std::optional<std::string> Catalog::ability_display_name(const std::string& ability_id) const {
    if (ability_id.empty()) return std::nullopt;
    if (const std::string* v = strings_.find("Abil/Name/" + ability_id)) return *v;
    return std::nullopt;
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

std::optional<Catalog::MergeSpec> Catalog::merge_spec(const std::string& abil_id) const {
    const Entry* e = find("CAbil", abil_id);
    if (!e || e->tag != "CAbilMerge") return std::nullopt;
    MergeSpec spec;
    for (const XmlNode& info : resolved("CAbil", abil_id)) {
        if (info.name != "Info") continue;
        if (const std::string* u = info.attr("Unit"); u && !u->empty()) spec.target = *u;
        if (const std::string* t = info.attr("Time")) spec.time = std::atof(t->c_str());
        for (const XmlNode& c : info.children) {
            if (c.name == "Unit") {
                if (const std::string* v = c.attr("value"); v && !v->empty()) spec.target = *v;
                else if (!c.text.empty()) spec.target = c.text;
            } else if (c.name == "Time") {
                if (const std::string* v = c.attr("value")) spec.time = std::atof(v->c_str());
                else if (!c.text.empty()) spec.time = std::atof(c.text.c_str());
            }
        }
    }
    if (spec.target.empty()) return std::nullopt;
    return spec;
}

std::optional<Catalog::BuildSpec> Catalog::build_spec(const std::string& abil_id) const {
    const Entry* e = find("CAbil", abil_id);
    if (!e || e->tag != "CAbilBuild") return std::nullopt;
    for (const XmlNode& info : resolved("CAbil", abil_id)) {
        if (info.name != "InfoArray") continue;
        const std::string built = info.attr_or("Unit", "");
        if (built.empty()) continue;
        double energy = 0;
        for (const XmlNode& g : info.children)
            if (g.name == "Vital" && g.attr_or("index", "") == "Energy") {
                const std::string v = g.attr_or("value", "");
                if (!v.empty()) energy = std::atof(v.c_str());
            }
        if (energy <= 0) continue;
        BuildSpec spec;
        spec.unit = built;
        spec.energy = energy;
        return spec;
    }
    return std::nullopt;
}

int Catalog::footprint_place_cells(const std::string& footprint_id) const {
    int count = 0;
    auto count_row = [&](const std::string& s) {
        for (char c : s)
            if (c != '.' && c != ' ' && c != '\t' && c != '\r' && c != '\n') ++count;
    };
    for (const XmlNode& layer : resolved("CFootprint", footprint_id)) {
        if (layer.name != "Layers" || layer.attr_or("index", "") != "Place") continue;
        for (const XmlNode& row : layer.children) {
            if (row.name != "Rows") continue;
            if (const std::string* v = row.attr("value")) count_row(*v);
            else count_row(row.text);
        }
    }
    return count;
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

std::array<float, 3> Catalog::unit_render_scale(const std::string& unit_id) const {
    const std::array<float, 3> kIdentity{1.0f, 1.0f, 1.0f};
    if (unit_id.empty()) return kIdentity;

    if (!actor_by_unit_built_) {
        actor_by_unit_built_ = true;
        for (const std::string& aid : ids("CActorUnit")) {
            const Entry* e = find("CActorUnit", aid);
            if (!e) continue;
            const std::string* un = e->attr("unitName");
            const std::string& key = (un && !un->empty()) ? *un : aid;
            actor_by_unit_.emplace(key, aid);
        }
    }
    auto it = actor_by_unit_.find(unit_id);
    if (it == actor_by_unit_.end()) return kIdentity;

    std::optional<std::string> raw = scalar("CActorUnit", it->second, "Scale");
    if (!raw || raw->empty()) return kIdentity;

    std::array<float, 3> out = kIdentity;
    std::size_t comps = 0;
    const char* p = raw->c_str();
    while (*p && comps < 3) {
        char* end = nullptr;
        const double v = std::strtod(p, &end);
        if (end == p) break;
        out[comps++] = float(v);
        p = end;
        while (*p == ',' || *p == ' ') ++p;
    }
    if (comps == 0) return kIdentity;
    if (comps == 1) out[1] = out[2] = out[0];
    return out;
}

}
