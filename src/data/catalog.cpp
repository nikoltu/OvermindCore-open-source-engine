#include <algorithm>
#include "omc/data/catalog.hpp"

#include <cstdio>
#include <cstdlib>
#include <functional>

#include "omc/casc/storage.hpp"
#include "omc/data/melee_mods.hpp"

namespace omc::data {

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
    if (tag.rfind("CTurret", 0) == 0) return "CTurret";
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
        if (add_only_) {
            const std::string* aid = e.attr("id");
            if (e.name == "const" || e.name.empty() || !aid) continue;
            const std::string fam_name = family_of(e.name);
            const std::string key = fam_name + std::string(1, '\x1f') + *aid;
            const auto fit = families_.find(fam_name);
            const bool existed = fit != families_.end() && fit->second.find(*aid) != fit->second.end();
            if (existed && add_only_new_.find(key) == add_only_new_.end()) continue;
            add_only_new_.insert(key);
        }
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
                auto& idefs = tag_default_indexed_[e.name];
                for (const XmlNode& field : e.children)
                    if (field.attr("index")) idefs.push_back(field);
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
            if (field.name == "?token") {
                const std::string* tid = field.attr("id");
                const std::string* tval = field.attr("value");
                if (tid && tval && !tid->empty()) {
                    bool found = false;
                    for (auto& kv : entry.attrs)
                        if (kv.first == *tid) { kv.second = *tval; found = true; break; }
                    if (!found) entry.attrs.emplace_back(*tid, *tval);
                }
                continue;
            }
            entry.own.push_back(field);
            entry.own_decl.push_back(decl);
        }
    }
    resolved_cache_.clear();
    resolved_copies_.clear();
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

std::string Catalog::sibling_actor_family(const std::string& family, const std::string& id) const {
    if (family.rfind("CActor", 0) != 0) return family;
    const std::string* best = nullptr;
    for (const auto& [name, ids] : families_)
        if (name != family && name.rfind("CActor", 0) == 0 && ids.count(id) && (!best || name < *best)) best = &name;
    return best ? *best : family;
}

const Catalog::Entry* Catalog::find(const std::string& family,
                                    const std::string& id) const {
    auto fit = families_.find(family);
    if (fit == families_.end()) return nullptr;
    auto it = fit->second.find(id);
    return it == fit->second.end() ? nullptr : &it->second;
}

Catalog::FieldReadLog Catalog::authored_field_pairs() const {
    FieldReadLog out;
    for (const auto& [family, idmap] : families_)
        for (const auto& [id, e] : idmap)
            for (const XmlNode& n : e.own) out.emplace(family, n.name);
    return out;
}

std::vector<std::string> Catalog::ids(const std::string& family) const {
    auto oit = family_order_.find(family);
    if (oit == family_order_.end()) return {};
    return oit->second;
}

const core::XmlNodeList& Catalog::resolved(const std::string& family, const std::string& id) const {
    const core::XmlNodeList& r = resolve_chain(family, id);
    if (field_touch_log_)
        for (const XmlNode& c : r) field_touch_log_->emplace(family, c.name);
    return r;
}

std::size_t Catalog::memory_bytes(std::size_t* entries, std::size_t* resolved_cache) const {
    std::size_t e = 0, r = 0;
    for (const auto& fam : families_)
        for (const auto& kv : fam.second) {
            e += sizeof(Entry) + 64 + kv.second.own.capacity() * sizeof(XmlNode);
            for (const XmlNode& n : kv.second.own) e += core::xml_node_heap_bytes(n);
        }
    for (const auto* m : {&singletons_, &tag_defaults_, &tag_default_indexed_})
        for (const auto& kv : *m)
            for (const XmlNode& n : kv.second) e += sizeof(XmlNode) + core::xml_node_heap_bytes(n);
    for (const auto& kv : resolved_cache_) r += 64 + kv.first.capacity() + kv.second.memory_bytes();
    for (const auto& kv : resolved_copies_)
        for (const XmlNode& n : kv.second) r += sizeof(XmlNode) + core::xml_node_heap_bytes(n);
    if (entries) *entries = e;
    if (resolved_cache) *resolved_cache = r;
    return e + r;
}

const core::XmlNodeList& Catalog::resolve_chain(const std::string& family, const std::string& id) const {
    std::string key = family + "\x1f" + id;
    auto cit = resolved_cache_.find(key);
    if (cit != resolved_cache_.end()) return cit->second;
    static const bool kCopies = std::getenv("OMC_CATALOG_RESOLVED_COPIES") != nullptr;

    core::XmlNodeList out;
    std::vector<XmlNode> copies;
    const Entry* e = find(family, id);
    if (e) {
        resolved_cache_.emplace(key, core::XmlNodeList{});
        if (!e->parent.empty() && e->parent != id) {
            const std::string& pf = find(family, e->parent) ? family : sibling_actor_family(family, e->parent);
            const auto& base = resolve_chain(pf, e->parent);
            if (kCopies) for (const XmlNode& n : base) copies.push_back(n);
            else out.append(base);
        }
        if (kCopies) copies.insert(copies.end(), e->own.begin(), e->own.end());
        else for (const XmlNode& n : e->own) out.push_back(&n);
    }
    if (kCopies) {
        std::vector<XmlNode>& held = resolved_copies_[key] = std::move(copies);
        for (const XmlNode& n : held) out.push_back(&n);
    }
    return resolved_cache_[key] = std::move(out);
}

std::optional<std::string> Catalog::scalar(const std::string& family,
                                           const std::string& id,
                                           const std::string& field) const {
    if (field_read_log_) field_read_log_->emplace(family, field);
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
    if (field_read_log_) field_read_log_->emplace(family, field);
    const Entry* e = find(family, id);
    if (!e) return {};

    std::vector<NodeSlot> base;
    static thread_local int parent_depth = 0;
    if (!e->parent.empty() && e->parent != id && parent_depth < 64) {
        ++parent_depth;
        try { base = resolve_node_slots(family, e->parent, field, self); } catch (...) { --parent_depth; throw; }
        --parent_depth;
    }

    struct Own { long key; bool is_pos; std::vector<SlotWrite> writes; bool dead; };
    std::vector<Own> own;
    std::unordered_map<long, size_t> own_at;
    long counter = 0;
    long next_append = 0;
    long cur_decl = -1;
    std::vector<long> decl_index_keys;
    static const bool kPlainOverIndex = std::getenv("OMC_ARRAY_PLAIN_OVER_INDEX") != nullptr;
    for (size_t k = 0; k < e->own.size(); ++k) {
        if (e->own[k].name != field) {
            if (k < e->own_decl.size() && long(e->own_decl[k]) != cur_decl) {
                cur_decl = long(e->own_decl[k]); counter = next_append;
                decl_index_keys.clear();
            }
            continue;
        }
        if (k < e->own_decl.size() && long(e->own_decl[k]) != cur_decl) {
            cur_decl = long(e->own_decl[k]); counter = next_append;
            decl_index_keys.clear();
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
        if (has_index) { key = index; is_pos = false; decl_index_keys.push_back(index); }
        else {
            if (!kPlainOverIndex)
                while (std::find(decl_index_keys.begin(), decl_index_keys.end(), counter) != decl_index_keys.end()) ++counter;
            key = counter; is_pos = true; ++counter;
        }
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

    static const bool kReviveOld = std::getenv("OMC_ARRAY_REVIVE_OLD") != nullptr;
    const size_t inherited = base.size();
    for (Own& o : own) {
        if (o.is_pos) {
            base.push_back(NodeSlot{std::move(o.writes), o.dead});
        } else {
            size_t idx = size_t(o.key < 0 ? 0 : o.key);
            if (idx > 65535) continue;
            if (idx >= base.size()) base.resize(idx + 1);
            NodeSlot& s = base[idx];
            s.writes.insert(s.writes.end(), o.writes.begin(), o.writes.end());
            if (o.dead) s.dead = true;
            else if (!kReviveOld && s.dead && idx < inherited)
                for (const SlotWrite& w : o.writes) {
                    const std::string* lv = w.node->attr("Link");
                    if (!lv) lv = w.node->attr("value");
                    if (lv && !lv->empty()) { s.dead = false; break; }
                }
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
            if (auto a = effect_amount(id)) return int(*a);
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
    if (auto d = scalar_num("CBehavior", behavior_id, "Duration")) { bb.duration = *d; bb.duration_authored = true; any = true; }
    if (auto ms = scalar_num("CBehavior", behavior_id, "MaxStackCount")) bb.max_stack = int(*ms);
    if (auto p = scalar_num("CBehavior", behavior_id, "Period")) bb.dmg_period = *p;
    if (auto pe = scalar("CBehavior", behavior_id, "PeriodicEffect"); pe && !pe->empty())
        if (auto d = effect_damage_f(*pe)) { bb.dmg_per_tick = *d; any = true; }
    bb.initial_effect = scalar("CBehavior", behavior_id, "InitialEffect").value_or("");
    bb.periodic_effect = scalar("CBehavior", behavior_id, "PeriodicEffect").value_or("");
    bb.expire_effect = scalar("CBehavior", behavior_id, "ExpireEffect").value_or("");
    bb.final_effect = scalar("CBehavior", behavior_id, "FinalEffect").value_or("");
    for (const core::XmlNode& n : resolved("CBehavior", behavior_id)) {
        if (n.name != "Modification") continue;
        any = true;
        if (const std::string* v = n.attr("MoveSpeedMultiplier"); v && !v->empty())
            bb.move_speed_mult = std::strtod(v->c_str(), nullptr);
        if (const std::string* v = n.attr("DecelerationMultiplier"); v && !v->empty())
            bb.decel_mult *= std::strtod(v->c_str(), nullptr);
        for (const core::XmlNode& g : n.children)
            if (g.name == "DecelerationMultiplier")
                if (const std::string v2 = g.attr_or("value", ""); !v2.empty())
                    bb.decel_mult *= std::strtod(v2.c_str(), nullptr);
        if (const std::string* v = n.attr("AccelerationMultiplier"); v && !v->empty())
            bb.accel_mult *= std::strtod(v->c_str(), nullptr);
        for (const core::XmlNode& g : n.children) {
            if (g.name == "AccelerationMultiplier")
                if (const std::string v2 = g.attr_or("value", ""); !v2.empty())
                    bb.accel_mult *= std::strtod(v2.c_str(), nullptr);
            if (g.name == "DamageDealtAttributeScaled") {
                const std::string idx = g.attr_or("index", "");
                const std::string v2 = g.attr_or("value", "");
                if (idx.empty() || v2.empty()) continue;
                const double amt = std::strtod(v2.c_str(), nullptr);
                bool put = false;
                for (auto& [a, x] : bb.damage_attr_bonus)
                    if (a == idx) { x = amt; put = true; }
                if (!put) bb.damage_attr_bonus.emplace_back(idx, amt);
            }
        }
        if (const std::string* v = n.attr("MoveSpeedMaximum"); v && !v->empty())
            bb.move_speed_max = std::strtod(v->c_str(), nullptr);
        for (const core::XmlNode& g : n.children)
            if (g.name == "MoveSpeedMaximum")
                if (const std::string v2 = g.attr_or("value", ""); !v2.empty())
                    bb.move_speed_max = std::strtod(v2.c_str(), nullptr);
        if (const std::string* v = n.attr("WeaponRange"); v && !v->empty())
            bb.weapon_range_bonus += std::strtod(v->c_str(), nullptr);
        for (const core::XmlNode& g : n.children)
            if (g.name == "WeaponRange")
                if (const std::string v2 = g.attr_or("value", ""); !v2.empty())
                    bb.weapon_range_bonus += std::strtod(v2.c_str(), nullptr);
        if (const std::string* v = n.attr("AttackSpeedMultiplier"); v && !v->empty())
            bb.attack_speed_mult = std::strtod(v->c_str(), nullptr);
        if (const std::string* v = n.attr("Food"); v && !v->empty()) bb.food = std::strtod(v->c_str(), nullptr);
        for (const core::XmlNode& g : n.children) {
            const std::string v2 = g.attr_or("value", "");
            if (v2.empty()) continue;
            if (g.name == "Food") bb.food = std::strtod(v2.c_str(), nullptr);
            else if (g.name == "VitalMaxArray" && g.attr_or("index", "") == "Life")
                bb.life_max_bonus = std::strtod(v2.c_str(), nullptr);
        }
        if (const std::string* v = n.attr("LifeArmorBonus"); v && !v->empty())
            bb.life_armor_bonus = std::strtod(v->c_str(), nullptr);
        if (const std::string* v = n.attr("ShieldArmorBonus"); v && !v->empty())
            bb.shield_armor_bonus = std::strtod(v->c_str(), nullptr);
        for (const core::XmlNode& g : n.children) {
            const std::string* v = g.attr("value");
            if (!v || v->empty()) continue;
            if (g.name == "LifeArmorBonus") bb.life_armor_bonus = std::strtod(v->c_str(), nullptr);
            if (g.name == "ShieldArmorBonus") bb.shield_armor_bonus = std::strtod(v->c_str(), nullptr);
        }
        for (const core::XmlNode& g : n.children)
            if (g.name == "RateMultiplierArray")
                if (const std::string* v = g.attr("value"); v && !v->empty()) {
                    const double r = std::strtod(v->c_str(), nullptr);
                    if (r > bb.rate_mult) bb.rate_mult = r;
                }
        for (const core::XmlNode& g : n.children)
            if (g.name == "StateFlags") {
                const std::string idx = g.attr_or("index", "");
                if (idx == "CanAttack" && g.attr_or("value", "") == "0") bb.root = false;
                const bool on = g.attr_or("value", "") == "1";
                if (idx == "Stun" && on) bb.stun = true;
                if (idx == "SuppressAttack" && on) bb.suppress_attack = true;
                if (idx == "Invulnerable" && on) bb.invulnerable = true;
                if (idx == "Uncommandable" && on) bb.uncommandable = true;
                if (idx == "Untargetable" && on) bb.untargetable = true;
                if (idx == "SuppressCloak" && on) bb.suppress_cloak = true;
                if (idx == "Cloak" && on) bb.cloak = true;
                if (idx == "SuppressCollision" && on) bb.suppress_collision = true;
                if (idx == "SuppressPassenger" && on) bb.suppress_passenger = true;
                if (idx == "Benign" && on) bb.benign = true;
                if (idx == "Passive" && on) bb.passive = true;
            }
        for (const core::XmlNode& g : n.children)
            if (g.name == "ModifyFlags" && g.attr_or("value", "") == "1") {
                const std::string idx = g.attr_or("index", "");
                if (idx == "DisableAbils") { bb.disable_abils = true; any = true; }
                if (idx == "SuppressMoving") { bb.root = true; any = true; }
            }
        for (const core::XmlNode& g : n.children)
            if (g.name == "AbilClassDisableArray" && g.attr_or("value", "") == "1") {
                const std::string idx = g.attr_or("index", "");
                if (idx == "CAbilAttack") bb.suppress_attack = true;
                if (idx == "CAbilMove") bb.root = true;
                if (idx == "CAbilTrain" || idx == "CAbilQueue" || idx == "CAbilWarpTrain") bb.disable_production = true;
                if (idx == "CAbilMorph" || idx == "CAbilMorphPlacement") bb.disable_morph = true;
                if (idx == "CAbilTransport") bb.disable_transport = true;
            } else if (g.name == "BehaviorLinkDisableArray" && g.attr_or("value", "") == "SpawnLarva") {
                bb.disable_spawner = true;
            } else if (g.name == "WeaponDisableArray") {
                const std::string v = g.attr_or("value", "");
                if (!v.empty()) bb.disabled_weapons.push_back(v);
            } else if (g.name == "AbilLinkDisableArray") {
                const std::string v = g.attr_or("value", "");
                if (!v.empty()) bb.disabled_abils.push_back(v);
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
        const std::string v = n.attr_or("value", "1");
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

std::vector<std::string> Catalog::morph_finish_autocast_off(const std::string& abil_id) const {
    std::vector<std::string> out;
    const Entry* e = find("CAbil", abil_id);
    if (!e || e->tag != "CAbilMorph") return out;
    std::vector<std::string> fins;
    for (const core::XmlNode& info : resolved("CAbil", abil_id)) {
        if (info.name != "InfoArray") continue;
        for (const core::XmlNode& sec : info.children) {
            if (sec.name != "SectionArray" || sec.attr_or("index", "") != "Abils") continue;
            for (const core::XmlNode& ea : sec.children)
                if (ea.name == "EffectArray" && ea.attr_or("index", "") == "Finish")
                    if (const std::string* v = ea.attr("value"); v && !v->empty()) fins.push_back(*v);
        }
    }
    for (const std::string& f : fins) {
        std::string beh = f;
        if (!find("CBehavior", beh)) {
            if (effect_subclass(f) != "ApplyBehavior") continue;
            beh = scalar("CEffect", f, "Behavior").value_or("");
            if (beh.empty() || !find("CBehavior", beh)) continue;
        }
        std::vector<std::string> effs = array_links("CBehavior", beh, "PeriodicEffectArray");
        for (const char* field : {"PeriodicEffect", "InitialEffect"})
            if (auto p = scalar("CBehavior", beh, field); p && !p->empty()) effs.push_back(*p);
        for (const std::string& eid : effs) {
            const Entry* ee = find("CEffect", eid);
            if (!ee || ee->tag != "CEffectIssueOrder") continue;
            bool set = false, on = false;
            for (const core::XmlNode& n : resolved("CEffect", eid)) {
                if (n.name != "CmdFlags") continue;
                const std::string idx = n.attr_or("index", "");
                const std::string v = n.attr_or("value", "1");
                const bool b = !v.empty() && v != "0";
                if (idx == "SetAutoCast") set = b;
                else if (idx == "SetAutoCastOn") on = b;
            }
            const std::string a = scalar("CEffect", eid, "Abil").value_or("");
            if (set && !on && !a.empty() && std::find(out.begin(), out.end(), a) == out.end()) out.push_back(a);
        }
    }
    return out;
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
        if (const std::string* h = n.attr("Handled"); h && !h->empty()) r.handled = *h;
        for (const core::XmlNode& cost : n.children) {
            if (cost.name != "Cost") continue;
            for (const core::XmlNode& cd : cost.children) {
                if (cd.name != "Cooldown") continue;
                if (auto v = field(cd, "TimeUse")) r.cooldown = *v;
            }
        }
        for (const core::XmlNode& k : n.children) {
            if (k.name == "Kind" && k.attr_or("index", "") == "Spell" && k.attr_or("value", "") == "0")
                r.excludes_spell = true;
            if (k.name != "Kind") continue;
            const std::string idx = k.attr_or("index", "");
            const std::uint8_t bit = idx == "Spell" ? 1 : idx == "Melee" ? 2 : idx == "Ranged" ? 4 : idx == "Splash" ? 8 : 0;
            if (bit) { if (k.attr_or("value", "1") == "0") r.kinds &= std::uint8_t(~bit); else r.kinds |= bit; }
        }
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
            if (auto r = scalar_num("CEffect", id, "SpawnRange"); r && *r > 0) s.spawn_range = *r;
            for (const core::XmlNode& n : resolved("CEffect", id)) {
                if (n.name != "SpawnOwner") continue;
                const std::string* v = n.attr("Value");
                if (!v) continue;
                if (*v == "Neutral") s.spawn_owner = SpawnOwner::Neutral;
                else if (*v == "Target") s.spawn_owner = SpawnOwner::Target;
                else if (*v == "Source") s.spawn_owner = SpawnOwner::Source;
                else s.spawn_owner = SpawnOwner::Caster;
            }
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
    else if (const Entry* me = find("CAbil", mag))
        if (auto d = tag_default_scalar(me->tag, "Leash"); d && !d->empty()) s.leash = std::strtod(d->c_str(), nullptr);
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
                    s.affiliation = effect_search_affiliation(pe);
                    s.excludes_structure = effect_search_excludes_structure(pe);
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
        if (auto amount = effect_amount(*leaf)) return int(*amount) * hits;
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
    if (auto v = effect_missile_speed(eff)) return v;
    static const bool no_burst = [] { const char* e = std::getenv("OMC_NO_BURST_MISSILE"); return e && e[0] == '1'; }();
    if (no_burst) return std::nullopt;
    const Entry* pe = find("CEffect", eff);
    if (!pe || pe->tag != "CEffectCreatePersistent") return std::nullopt;
    for (const std::string& lm : array_links("CEffect", eff, "PeriodicEffectArray"))
        if (auto v = effect_missile_speed(lm)) return v;
    return std::nullopt;
}

std::optional<double> Catalog::effect_missile_speed(const std::string& eff) const {
    const std::vector<MissileMover> movers = effect_missile_movers(eff);
    if (movers.empty()) return std::nullopt;
    const MissileMover& m = movers.front();
    if (m.max_speed > 0.0) return m.max_speed;
    if (m.speed > 0.0) return m.speed;
    return std::nullopt;
}

double Catalog::weapon_turret_yaw_rate(const std::string& unit_id) const {
    for (const std::string& w : array_links("CUnit", unit_id, "WeaponArray")) {
        const std::string tid = weapon_turret(unit_id, w);
        if (tid.empty()) continue;
        if (const auto ts = turret(tid)) return ts->yaw_rate;
    }
    return 0.0;
}

std::vector<Catalog::MissileMover> Catalog::effect_missile_movers(const std::string& eff, const char* field) const {
    std::vector<MissileMover> out;
    const Entry* ee = find("CEffect", eff);
    if (!ee || ee->tag != "CEffectLaunchMissile") return out;
    auto kinematics = [&](const std::string& mover, double range_lte) {
        MissileMover m;
        m.range_lte = range_lte;
        bool any = false;
        int ordinal = 0;
        for (const core::XmlNode& n : resolved("CMover", mover)) {
            if (n.name != "MotionPhases") continue;
            int phase = ordinal++;
            if (const std::string* ix = n.attr("index")) {
                try { phase = std::stoi(*ix); } catch (...) {}
                --ordinal;
            }
            if (phase != 0) continue;
            for (const core::XmlNode& c : n.children) {
                const std::string* v = c.attr("value");
                if (!v || v->empty()) continue;
                double d = 0.0;
                try { d = std::stod(*v); } catch (...) { continue; }
                if (c.name == "Speed") { m.speed = d; any = true; }
                else if (c.name == "MaxSpeed") { m.max_speed = d; any = true; }
                else if (c.name == "Acceleration") m.accel = d;
            }
        }
        if (any) out.push_back(m);
    };
    const std::string fld = field ? field : "Movers";
    for (const core::XmlNode& n : resolved("CEffect", eff)) {
        if (n.name != fld) continue;
        const std::string* link = n.attr("Link");
        if (!link || link->empty()) link = n.attr("value");
        if (!link || link->empty()) continue;
        double lte = 0.0;
        if (const std::string* r = n.attr("IfRangeLTE")) { try { lte = std::stod(*r); } catch (...) {} }
        kinematics(*link, lte);
    }
    if (!out.empty() || fld != "Movers") return out;
    std::string mover;
    {
        std::string au = scalar("CEffect", eff, "AmmoUnit").value_or("");
        if (au.empty()) au = eff + "Weapon";
        if (find("CUnit", au)) {
            if (auto m = scalar("CUnit", au, "Mover"); m && !m->empty()) mover = *m;
            else if (find("CMover", au)) mover = au;
        }
    }
    if (!mover.empty()) kinematics(mover, 0.0);
    return out;
}

std::optional<std::string> Catalog::weapon_missile_model_id(const std::string& weapon_id) const {
    const std::string eff = weapon_effect_id(weapon_id);
    if (eff.empty()) return std::nullopt;
    const Entry* ee = find("CEffect", eff);
    if (!ee || ee->tag != "CEffectLaunchMissile") return std::nullopt;
    for (const std::string& act : ids("CActorAction")) {
        std::string launch;
        for (const Entry* e = find("CActorAction", act); e && launch.empty();
             e = e->parent.empty() ? nullptr : find("CActorAction", e->parent))
            if (const std::string* v = e->attr("effectLaunch")) launch = *v;
        if (launch != eff) continue;
        auto missile = scalar("CActorAction", act, "Missile");
        static const bool kTrace = std::getenv("OMC_SHOT_TRACE") != nullptr;
        if (kTrace)
            std::fprintf(stderr, "[shotmodel] weapon '%s' effect '%s' action '%s' Missile '%s' model '%s'\n", weapon_id.c_str(), eff.c_str(), act.c_str(),
                         missile ? missile->c_str() : "(none)", missile ? scalar("CActorMissile", *missile, "Model").value_or("(none)").c_str() : "-");
        if (!missile || missile->empty()) continue;
        auto model = scalar("CActorMissile", *missile, "Model");
        if (!model || model->empty()) continue;
        std::string id = *model;
        const std::string tok = "##unitName##";
        for (std::size_t at = id.find(tok); at != std::string::npos; at = id.find(tok, at))
            id.replace(at, tok.size(), actor_unit_name(*missile, "CActorMissile"));
        if (auto path = scalar("CModel", id, "Model"); path && !path->empty()) return id;
    }
    static const bool kAmmoOld = std::getenv("OMC_EFFECT_MISSILE_OLD") != nullptr;
    const std::string ammo = kAmmoOld ? std::string() : scalar("CEffect", eff, "AmmoUnit").value_or("");
    if (!ammo.empty())
        for (const std::string& act : ids("CActorMissile")) {
            if (actor_unit_name(act, "CActorMissile") != ammo) continue;
            auto model = scalar("CActorMissile", act, "Model");
            if (!model || model->empty()) continue;
            std::string id = *model;
            const std::string tok = "##unitName##";
            for (std::size_t at = id.find(tok); at != std::string::npos; at = id.find(tok, at)) id.replace(at, tok.size(), ammo);
            if (auto path = scalar("CModel", id, "Model"); path && !path->empty()) return id;
        }
    return std::nullopt;
}

static std::vector<std::string> effect_leaves(const Catalog& cat, const std::string& root) {
    std::vector<std::string> out;
    std::vector<std::pair<std::string, int>> todo{{root, 0}};
    while (!todo.empty()) {
        const auto [eff, depth] = todo.back();
        todo.pop_back();
        if (eff.empty() || std::find(out.begin(), out.end(), eff) != out.end()) continue;
        out.push_back(eff);
        if (depth >= 4) continue;
        const Catalog::Entry* e = cat.find("CEffect", eff);
        if (!e || (e->tag != "CEffectSwitch" && e->tag != "CEffectSet" && e->tag != "CEffectRandom")) continue;
        for (const core::XmlNode& n : cat.resolved("CEffect", eff)) {
            if (n.name == "CaseArray" || n.name == "CaseDefault" || n.name == "EffectArray") {
                const std::string* v = n.attr(n.name == "CaseArray" ? "Effect" : "value");
                if (v && !v->empty()) todo.push_back({*v, depth + 1});
            }
        }
    }
    return out;
}

std::optional<std::string> Catalog::weapon_impact_model_id(const std::string& weapon_id) const {
    if (weapon_id.empty()) return std::nullopt;
    const std::string eff = weapon_effect_id(weapon_id);
    static const bool kLeafOld = std::getenv("OMC_IMPACT_LEAF_OLD") != nullptr;
    const std::vector<std::string> leaves = kLeafOld ? std::vector<std::string>{} : effect_leaves(*this, eff);
    for (const std::string& act : ids("CActorAction")) {
        std::string attack, launch;
        for (const Entry* e = find("CActorAction", act); e; e = e->parent.empty() ? nullptr : find("CActorAction", e->parent)) {
            if (const std::string* v = e->attr("effectAttack"); v && attack.empty()) attack = *v;
            if (const std::string* v = e->attr("effectLaunch"); v && launch.empty()) launch = *v;
        }
        static const bool kAttackOnly = std::getenv("OMC_IMPACT_ATTACK_ONLY") != nullptr;
        const bool by_attack = !attack.empty() && (attack == weapon_id || attack == eff || std::find(leaves.begin(), leaves.end(), attack) != leaves.end());
        const bool by_missile = !kAttackOnly && !launch.empty() && launch == eff;
        if (!by_attack && !by_missile) continue;
        std::string model;
        for (const XmlNode& n : resolved("CActorAction", act))
            if (n.name == "ImpactMap" && n.attr_or("index", "") == "None")
                if (const std::string* m = n.attr("Model")) model = *m;
        if (model.empty()) continue;
        const std::string tok = "##id##";
        for (std::size_t at = model.find(tok); at != std::string::npos; at = model.find(tok, at))
            model.replace(at, tok.size(), act);
        if (auto path = scalar("CModel", model, "Model"); path && !path->empty()) return model;
    }
    return std::nullopt;
}

std::optional<std::string> Catalog::weapon_launch_model_id(const std::string& weapon_id) const {
    if (weapon_id.empty()) return std::nullopt;
    const std::string eff = weapon_effect_id(weapon_id);
    static const bool kLeafOld = std::getenv("OMC_IMPACT_LEAF_OLD") != nullptr;
    const std::vector<std::string> leaves = kLeafOld ? std::vector<std::string>{} : effect_leaves(*this, eff);
    for (const std::string& act : ids("CActorAction")) {
        std::string attack;
        for (const Entry* e = find("CActorAction", act); e && attack.empty();
             e = e->parent.empty() ? nullptr : find("CActorAction", e->parent))
            if (const std::string* v = e->attr("effectAttack")) attack = *v;
        if (attack.empty() || (attack != weapon_id && attack != eff && std::find(leaves.begin(), leaves.end(), attack) == leaves.end())) continue;
        std::string model;
        for (const XmlNode& n : resolved("CActorAction", act))
            if (n.name == "LaunchAssets")
                if (const std::string* m = n.attr("Model")) model = *m;
        if (model.empty()) continue;
        const std::string tok = "##id##";
        for (std::size_t at = model.find(tok); at != std::string::npos; at = model.find(tok, at))
            model.replace(at, tok.size(), act);
        if (auto path = scalar("CModel", model, "Model"); path && !path->empty()) return model;
    }
    return std::nullopt;
}

std::string Catalog::ability_command_requirement(const std::string& abil_id,
                                                 const std::string& cmd_index) const {
    static const bool kClearOld = std::getenv("OMC_CMD_REQ_CLEAR_OLD") != nullptr;
    std::string req;
    for (const core::XmlNode& n : resolved("CAbil", abil_id)) {
        if (n.name != "CmdButtonArray") continue;
        if (n.attr_or("index", "") != cmd_index) continue;
        const std::string* r = n.attr("Requirements");
        if (!r) continue;
        if (!r->empty() || !kClearOld) req = *r;
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

std::string Catalog::power_link(const std::string& beh) const {
    const auto v = scalar("CBehavior", beh, "PowerLink");
    if (!v || v->empty() || v->find("##") != std::string::npos) return beh;
    return *v;
}

int Catalog::power_source_level(const std::string& beh) const {
    const auto v = scalar_num("CBehavior", beh, "PowerLevel");
    return v && *v >= 1 ? int(*v) : 1;
}

bool Catalog::power_field_link(const std::string& link) const {
    if (!power_field_links_built_) {
        for (const std::string& id : ids("CBehavior")) {
            const Entry* e = find("CBehavior", id);
            if (!e || e->tag != "CBehaviorPowerUser") continue;
            const auto lvl = scalar_num("CBehavior", id, "PlacementMinPowerLevel");
            if (lvl && *lvl > 0) power_field_links_.insert(power_link(id));
        }
        power_field_links_built_ = true;
    }
    return power_field_links_.count(link) != 0;
}

Catalog::UnitPower Catalog::unit_power(const std::string& unit_id) const {
    UnitPower out;
    std::vector<std::string> field_sources;
    std::unordered_set<std::string> base_off, boost_on, boost_off;
    int boost_min = 0;
    for (const std::string& beh : array_links("CUnit", unit_id, "BehaviorArray")) {
        if (beh.empty()) continue;
        const Entry* e = find("CBehavior", beh);
        if (!e) continue;
        if (e->tag == "CBehaviorPowerSource") {
            if (power_field_link(power_link(beh))) {
                field_sources.push_back(beh);
                if (auto r = scalar_num("CBehavior", beh, "Radius"); r && *r > out.radius) out.radius = *r;
            } else if (!scalar("CBehavior", beh, "Requirements")) {
                if (auto r = scalar_num("CBehavior", beh, "Radius"); r && *r > out.enabler_radius)
                    out.enabler_radius = *r;
                out.enabler_level = std::max(out.enabler_level, power_source_level(beh));
            }
            continue;
        }
        if (e->tag != "CBehaviorPowerUser" || power_field_link(power_link(beh))) continue;
        for (const XmlNode& st : resolved("CBehavior", beh)) {
            if (st.name != "PowerStageArray") continue;
            const int min_level = std::atoi(st.attr_or("MinPowerLevel", "0").c_str());
            for (const XmlNode& m : st.children) {
                if (m.name != "Modification") continue;
                for (const XmlNode& f : m.children) {
                    const std::string v = f.attr_or("value", "");
                    if (v.empty()) continue;
                    const bool on = f.name == "BehaviorLinkEnableArray";
                    if (!on && f.name != "BehaviorLinkDisableArray") continue;
                    if (min_level <= 0) {
                        if (!on) base_off.insert(v);
                    } else {
                        (on ? boost_on : boost_off).insert(v);
                        boost_min = boost_min == 0 ? min_level : std::min(boost_min, min_level);
                    }
                }
            }
        }
    }
    for (const std::string& s : field_sources) {
        if (!base_off.count(s)) out.level = std::max(out.level, power_source_level(s));
        const bool boosted_on = boost_on.count(s) || (!base_off.count(s) && !boost_off.count(s));
        if (boosted_on) out.boosted_level = std::max(out.boosted_level, power_source_level(s));
    }
    if (out.radius > 0 && out.level == 0 && out.boosted_level == 0) out.level = 1;
    if (boost_min > 0 && out.boosted_level > out.level) out.boost_min_level = boost_min;
    else out.boosted_level = out.level;
    return out;
}

double Catalog::warp_progress_rate(const std::string& unit_id, int power_level) const {
    const std::string beh = warpable_power_behavior(unit_id);
    if (beh.empty()) return 1.0;
    double rate = 1.0;
    int best = -1;
    for (const XmlNode& st : resolved("CBehavior", beh)) {
        if (st.name != "PowerStageArray") continue;
        const int min_level = std::atoi(st.attr_or("MinPowerLevel", "0").c_str());
        if (min_level > power_level || min_level < best) continue;
        double r = 1.0;
        for (const XmlNode& m : st.children) {
            if (m.name != "Modification") continue;
            for (const XmlNode& f : m.children)
                if (f.name == "RateMultiplierArray" && f.attr_or("index", "") == "Progress")
                    if (const std::string* v = f.attr("value"))
                        if (const double d = std::strtod(v->c_str(), nullptr); d > 0) r = d;
        }
        best = min_level;
        rate = r;
    }
    return rate;
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
        if (const std::string* v = n.attr(key)) {
            dst = *v;
            return;
        }
        for (const core::XmlNode& ch : n.children)
            if (ch.name == key) dst = ch.attr_or("value", dst);
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
                if (key > 65535) continue;
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

std::optional<Catalog::CursorLook> Catalog::cursor_look(const std::string& cursor_id) const {
    CursorLook look;
    for (const core::XmlNode& n : resolved("CCursor", cursor_id)) {
        const std::string v = n.attr_or("value", "");
        if (v.empty()) continue;
        if (n.name == "Texture") look.texture = v;
        else if (n.name == "HotspotX") look.hotspot_x = std::atoi(v.c_str());
        else if (n.name == "HotspotY") look.hotspot_y = std::atoi(v.c_str());
    }
    if (look.texture.empty()) return std::nullopt;
    return look;
}

std::optional<std::string> Catalog::behavior_info_icon(const std::string& behavior_id) const {
    std::optional<std::string> icon;
    bool hidden = false;
    for (const core::XmlNode& n : resolved("CBehavior", behavior_id)) {
        if (n.name == "InfoIcon") {
            const std::string v = n.attr_or("value", "");
            if (!v.empty()) icon = v;
        } else if (n.name == "InfoFlags" && n.attr_or("index", "") == "Hidden") {
            hidden = n.attr_or("value", "0") != "0";
        }
    }
    if (hidden) return std::nullopt;
    return icon;
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
                       "ConsoleSkinData.xml",
                       "ModelData.xml", "SoundData.xml",
                       "TerrainData.xml",
                       "LightData.xml",
                       "GameData.xml", "RaceData.xml",
                       "GameUIData.xml",
                       "CameraData.xml",
                       "AlertData.xml",
                       "CursorData.xml",
                       "SoundtrackData.xml",
                       "TurretData.xml",
                       "FoWData.xml"});
    load_strings(store);
    load_hotkeys(store, {});
    load_assets(store);
    load_ai_scripts(store);
}

void Catalog::load_ai_scripts(const casc::Storage& store) {
    for (const char* race : {"Terran", "Protoss", "Zerg"})
        for (const char* tier : {"High", "VyHd"}) {
            const std::string path = std::string("mods/void.sc2mod/base.sc2data/TriggerLibs/") + race + "/" + race + tier + ".galaxy";
            try {
                const std::vector<std::uint8_t> b = store.read_by_name(path);
                ai_scripts_[path] = std::string(b.begin(), b.end());
            } catch (const std::exception&) {
            }
        }
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

void Catalog::load_missing_only(const casc::Storage& store, const std::vector<std::string>& roots) {
    std::vector<std::string> basenames(std::begin(kCampaignFiles), std::end(kCampaignFiles));
    add_only_ = true;
    try {
        load_root_files(store, roots, basenames);
    } catch (...) {
        add_only_ = false;
        throw;
    }
    add_only_ = false;
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

void Catalog::load_assets(const casc::Storage& store) {
    for (const char* mod : kMeleeMods) {
        std::string name = std::string("mods/") + mod + ".sc2mod/base.sc2data/GameData/Assets.txt";
        try {
            assets_.load(store.read_by_name(name));
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

void Catalog::load_hotkeys(const casc::Storage& store, const std::vector<std::string>& roots) {
    (void)roots;
    for (const char* mod : kMeleeMods) {
        std::string name = std::string("mods/") + mod +
                           ".sc2mod/enus.sc2data/LocalizedData/GameHotkeys.txt";
        try {
            hotkeys_.load(store.read_by_name(name));
        } catch (const std::exception&) {
        }
    }
}

static std::optional<std::string> button_hotkey_walk(const Catalog& cat, const StringTable& hk,
                                                     const std::string& face,
                                                     std::vector<std::string>& seen) {
    if (face.empty()) return std::nullopt;
    for (const std::string& s2 : seen)
        if (s2 == face) return std::nullopt;
    seen.push_back(face);
    const Catalog::Entry* e = cat.find("CButton", face);
    if (auto ref = cat.scalar("CButton", face, "Hotkey"); ref && !ref->empty())
        if (const std::string* v = hk.find(*ref)) return *v;
    if (const std::string* v = hk.find("Button/Hotkey/" + face)) return *v;
    if (e && !e->parent.empty()) return button_hotkey_walk(cat, hk, e->parent, seen);
    return std::nullopt;
}

std::optional<std::string> Catalog::button_hotkey(const std::string& button_id) const {
    if (button_id.empty() || hotkeys_.empty()) return std::nullopt;
    std::vector<std::string> seen;
    return button_hotkey_walk(*this, hotkeys_, button_id, seen);
}

std::string Catalog::expand_entry_tokens(const std::string& family, const std::string& id,
                                         std::string v) const {
    if (v.find("##") == std::string::npos) return v;
    std::vector<std::pair<std::string, std::string>> tokens;
    std::string cur = id;
    for (int depth = 0; depth < 32 && !cur.empty(); ++depth) {
        const Entry* e = find(family, cur);
        if (!e) break;
        for (const auto& kv : e->attrs) {
            bool have = false;
            for (const auto& t : tokens)
                if (t.first == kv.first) { have = true; break; }
            if (!have) tokens.push_back(kv);
        }
        cur = e->parent;
    }
    for (const auto& t : tokens) {
        const std::string needle = "##" + t.first + "##";
        for (std::size_t p; (p = v.find(needle)) != std::string::npos;)
            v.replace(p, needle.size(), t.second);
    }
    return expand_id(std::move(v), id);
}

Catalog::ConsoleSkinModels Catalog::console_skin_models(const std::string& skin_id, const std::string& race) const {
    ConsoleSkinModels m;
    auto triple = [](const std::string& v, float (&out)[3]) {
        if (v.empty()) return;
        std::size_t i = 0;
        for (int k = 0; k < 3 && i <= v.size(); ++k) {
            const std::size_t c = v.find(',', i);
            const std::string field = v.substr(i, c == std::string::npos ? c : c - i);
            try {
                out[k] = std::stof(field);
            } catch (...) {
            }
            if (c == std::string::npos) break;
            i = c + 1;
        }
    };
    auto take = [&](const core::XmlNode& n, std::string& path, float (&pos)[3], float (&scl)[3]) {
        const std::string model = n.attr_or("Model", "");
        if (model.empty()) return;
        const std::string ref = expand_entry_tokens("CConsoleSkin", skin_id, model);
        path = race.empty() ? resolve_asset(ref) : resolve_asset(ref, race);
        triple(n.attr_or("Position", ""), pos);
        triple(n.attr_or("Scale", ""), scl);
    };
    for (const core::XmlNode& n : resolved("CConsoleSkin", skin_id)) {
        if (n.name == "MinimapPanelModel") take(n, m.minimap, m.minimap_pos, m.minimap_scale);
        else if (n.name == "InfoPanelModel") take(n, m.info, m.info_pos, m.info_scale);
        else if (n.name == "CommandPanelModel") take(n, m.command, m.command_pos, m.command_scale);
    }
    return m;
}

std::optional<std::string> Catalog::console_skin_featured_image(const std::string& skin_id) const {
    std::optional<std::string> img;
    for (const core::XmlNode& n : resolved("CConsoleSkin", skin_id))
        if (n.name == "FeaturedImage") {
            const std::string v = n.attr_or("value", "");
            if (!v.empty())
                img = resolve_asset(expand_entry_tokens("CConsoleSkin", skin_id, v));
        }
    return img;
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
    return morph_spec(abil_id, MorphFieldFold());
}

std::optional<Catalog::MorphSpec> Catalog::morph_spec(const std::string& abil_id, const MorphFieldFold& fold) const {
    const Entry* e = find("CAbil", abil_id);
    if (!e || (e->tag != "CAbilMorph" && e->tag != "CAbilMorphPlacement")) return std::nullopt;

    struct Section { std::string name; double delay = 0, dur = 0; };
    struct Stage { std::string unit; std::vector<Section> secs; Section actor{"Actor", 0, 0}; int slot = 0; double rd_min = 0, rd_max = 0; };
    std::vector<Stage> stages;
    std::string finish_effect;
    bool transient = false;
    int autopos = 0;
    std::vector<std::string> beh_on, beh_off;
    auto stage_at = [&](int slot) -> Stage& {
        if (slot < 0) slot = 0;
        if (slot > 65535) slot = 65535;
        if (int(stages.size()) <= slot) stages.resize(slot + 1);
        stages[slot].slot = slot;
        return stages[slot];
    };
    for (const XmlNode& info : resolved("CAbil", abil_id)) {
        if (info.name == "Flags" && info.attr_or("index", "") == "Transient") transient = info.attr_or("value", "0") != "0";
        if (info.name == "BehaviorOn" || info.name == "BehaviorOff") {
            const std::string v = info.attr_or("value", "");
            std::vector<std::string>& dst = info.name == "BehaviorOn" ? beh_on : beh_off;
            if (!v.empty() && std::find(dst.begin(), dst.end(), v) == dst.end()) dst.push_back(v);
        }
        if (info.name != "InfoArray") continue;
        const std::string* idxa = info.attr("index");
        Stage& st = stage_at(idxa ? std::atoi(idxa->c_str()) : autopos++);
        if (const std::string* u = info.attr("Unit"); u && !u->empty()) st.unit = *u;
        if (const std::string* v = info.attr("RandomDelayMin")) st.rd_min = std::atof(v->c_str());
        if (const std::string* v = info.attr("RandomDelayMax")) st.rd_max = std::atof(v->c_str());
        for (const XmlNode& c : info.children) {
            if (c.name != "RandomDelayMin" && c.name != "RandomDelayMax") continue;
            const std::string* v = c.attr("value");
            if (!v) continue;
            (c.name == "RandomDelayMin" ? st.rd_min : st.rd_max) = std::atof(v->c_str());
        }
        for (const XmlNode& sec : info.children) {
            if (sec.name != "SectionArray") continue;
            const std::string sname = sec.attr_or("index", "");
            for (const XmlNode& ef : sec.children)
                if (ef.name == "EffectArray" && ef.attr_or("index", "") == "Finish")
                    if (const std::string* v = ef.attr("value"); v && !v->empty()) finish_effect = *v;
            Section* slot = nullptr;
            if (sname == "Actor") slot = &st.actor;
            else
                for (Section& s : st.secs)
                    if (s.name == sname) { slot = &s; break; }
            if (!slot) { st.secs.push_back({sname, 0, 0}); slot = &st.secs.back(); }
            for (const XmlNode& d : sec.children) {
                if (d.name != "DurationArray") continue;
                const std::string* v = d.attr("value");
                const double val = v ? std::atof(v->c_str()) : 0.0;
                const std::string idx = d.attr_or("index", "");
                if (idx == "Delay") slot->delay = val;
                else if (idx == "Duration") slot->dur = val;
            }
        }
    }

    static const bool kDropRepeats = std::getenv("OMC_MORPH_REPEATED_STAGES") == nullptr;
    MorphSpec spec;
    const Stage* prev = nullptr;
    for (const Stage& st : stages) {
        if (st.unit.empty() || st.unit.find("##") != std::string::npos) continue;
        static const bool kSameUnitOne = std::getenv("OMC_MORPH_SAME_UNIT_STAGES") == nullptr;
        if (kDropRepeats && kSameUnitOne && prev && prev->unit == st.unit) continue;
        static const bool kChainOnce = std::getenv("OMC_MORPH_REPEATED_CHAIN") == nullptr;
        if (kDropRepeats && kChainOnce && std::find(spec.chain.begin(), spec.chain.end(), st.unit) != spec.chain.end()) continue;
        if (kDropRepeats && prev && prev->unit == st.unit && prev->secs.size() == st.secs.size()) {
            bool same = true;
            for (std::size_t k = 0; k < st.secs.size() && same; ++k)
                same = prev->secs[k].name == st.secs[k].name && prev->secs[k].delay == st.secs[k].delay && prev->secs[k].dur == st.secs[k].dur;
            if (same) continue;
        }
        prev = &st;
        auto timer = [&](const std::string& sec, const char* which, double v) {
            if (!fold) return v;
            const double f = fold("InfoArray[" + std::to_string(st.slot) + "].SectionArray[" + sec + "].DurationArray[" + which + "]", v);
            return f > 0.0 ? f : 0.0;
        };
        double stage_time = 0.0, order_stage = 0.0, abils_delay = 0.0, stats_end = 0.0;
        spec.stats_delay = 0.0;
        spec.collide_delay = 0.0;
        for (const Section& s : st.secs) {
            const double d = timer(s.name, "Delay", s.delay), du = timer(s.name, "Duration", s.dur);
            if (d + du > stage_time) stage_time = d + du;
            if (s.name == "Stats") {
                spec.stats_delay = d;
                stats_end = d + du;
            }
            if (s.name == "Collide") spec.collide_delay = d;
            if (s.name == "Abils") abils_delay = d;
        }
        order_stage = std::max(stage_time, timer("Actor", "Delay", st.actor.delay) + timer("Actor", "Duration", st.actor.dur));
        if (spec.chain.empty()) {
            spec.random_delay_min = st.rd_min;
            spec.random_delay_max = st.rd_max;
        }
        spec.stages.push_back({st.unit, spec.morph_time + abils_delay, spec.morph_time + std::max(abils_delay, stats_end)});
        spec.chain.push_back(st.unit);
        spec.morph_time += stage_time;
        spec.order_time += order_stage;
    }
    if (spec.chain.empty()) return std::nullopt;
    spec.target = spec.chain.back();
    spec.finish_effect = finish_effect;
    spec.transient = transient;
    spec.behavior_on = std::move(beh_on);
    spec.behavior_off = std::move(beh_off);
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
    return int(footprint_place_offsets(footprint_id).size());
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
    {
        auto parse_xy = [](const std::string& v, std::pair<double, double>& out) {
            const std::size_t c = v.find(',');
            if (c == std::string::npos) return false;
            out.first = std::atof(v.substr(0, c).c_str());
            out.second = std::atof(v.substr(c + 1).c_str());
            return true;
        };
        int seq = 0;
        for (const XmlNode& n : resolved("CBehavior", behavior_id)) {
            const std::string* v = n.attr("value");
            if (!v || v->empty()) continue;
            if (n.name == "Center") {
                parse_xy(*v, sb.center);
            } else if (n.name == "Offset") {
                const std::string* idxa = n.attr("index");
                const int slot = idxa ? std::atoi(idxa->c_str()) : seq++;
                if (slot < 0 || slot > 4095) continue;
                if (int(sb.offsets.size()) <= slot) sb.offsets.resize(std::size_t(slot) + 1);
                parse_xy(*v, sb.offsets[std::size_t(slot)]);
            }
        }
    }
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

std::optional<std::string> Catalog::unit_wireframe_image(const std::string& unit_id) const {
    if (unit_id.empty()) return std::nullopt;
    (void)unit_render_scale(unit_id);
    auto it = actor_by_unit_.find(unit_id);
    if (it == actor_by_unit_.end()) return std::nullopt;
    for (const core::XmlNode& n : resolved("CActorUnit", it->second)) {
        if (n.name != "Wireframe") continue;
        for (const core::XmlNode& c : n.children)
            if (c.name == "Image") {
                const std::string v = c.attr_or("value", "");
                if (!v.empty()) return v;
            }
        const std::string v = n.attr_or("value", "");
        if (!v.empty()) return v;
    }
    return std::nullopt;
}

static std::string on_trim(const std::string& s) {
    std::size_t a = 0, b = s.size();
    auto space = [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
    while (a < b && space(s[a])) ++a;
    while (b > a && space(s[b - 1])) --b;
    return s.substr(a, b - a);
}

static std::vector<std::string> on_split(const std::string& s, char sep) {
    std::vector<std::string> out;
    std::size_t start = 0;
    for (std::size_t i = 0; i <= s.size(); ++i) {
        if (i == s.size() || s[i] == sep) {
            out.push_back(s.substr(start, i - start));
            start = i + 1;
        }
    }
    return out;
}

namespace {

bool macro_ident_char(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}

void replace_token(std::string& s, const std::string& from, const std::string& to) {
    if (from.empty()) return;
    for (std::size_t p; (p = s.find(from)) != std::string::npos;)
        s.replace(p, from.size(), to);
}

}

std::string Catalog::expand_actor_tokens(const std::string& in, const std::string& unit_name,
                                         const std::string& self_id) const {
    if (in.find('$') == std::string::npos && in.find("##") == std::string::npos) return in;
    std::string out;
    out.reserve(in.size());
    for (std::size_t i = 0; i < in.size();) {
        if (in[i] == '$') {
            std::size_t j = i + 1;
            while (j < in.size() && macro_ident_char(in[j])) ++j;
            if (j > i + 1) {
                if (const std::string* v = const_value(in.substr(i, j - i))) {
                    out += *v;
                    i = j;
                    continue;
                }
            }
        }
        out.push_back(in[i++]);
    }
    if (!unit_name.empty()) replace_token(out, "##unitName##", unit_name);
    if (!self_id.empty()) replace_token(out, "##id##", self_id);
    return out;
}

std::string Catalog::actor_unit_name(const std::string& actor_id,
                                     const std::string& family) const {
    if (const Entry* e = find(family, actor_id))
        if (const std::string* un = e->attr("unitName"))
            if (!un->empty()) return *un;
    return actor_id;
}

std::vector<Catalog::ActorRule> Catalog::actor_rules(const std::string& actor_id,
                                                     const std::string& family, bool with_macros) const {
    std::vector<ActorRule> out;
    if (actor_id.empty()) return out;
    const std::string unit_name = actor_unit_name(actor_id, family);
    std::vector<NodeSlot> on_slots = resolve_node_slots(family, actor_id, "On");
    static const bool kMacros = std::getenv("OMC_NO_ACTOR_MACROS") == nullptr;
    if (kMacros && with_macros) {
        std::vector<std::string> mids;
        for (const NodeSlot& ms : resolve_node_slots(family, actor_id, "Macros")) {
            if (ms.dead) continue;
            for (const SlotWrite& w : ms.writes)
                if (const std::string* v = w.node->attr("value"); v && !v->empty() && std::find(mids.begin(), mids.end(), *v) == mids.end()) mids.push_back(*v);
        }
        for (const std::string& mid : mids) {
            std::vector<NodeSlot> mrows = resolve_node_slots("CActorEventMacro", mid, "On");
            if (std::getenv("OMC_MACRO_TRACE"))
                std::fprintf(stderr, "[macro] %s: %s -> %zu rows\n", actor_id.c_str(), mid.c_str(), mrows.size());
            for (NodeSlot& mo : mrows) on_slots.push_back(std::move(mo));
        }
    }
    for (const NodeSlot& slot : on_slots) {
        if (slot.dead) continue;
        const std::string* terms = nullptr;
        const std::string* send = nullptr;
        const std::string* target = nullptr;
        for (const SlotWrite& w : slot.writes) {
            if (const std::string* t = w.node->attr("Terms")) terms = t;
            if (const std::string* v = w.node->attr("Send")) send = v;
            if (const std::string* g = w.node->attr("Target")) target = g;
        }
        if (!terms && !send) continue;

        ActorRule r;
        if (target) r.target = expand_actor_tokens(*target, unit_name, actor_id);
        if (terms) {
            const std::string terms_x = expand_actor_tokens(*terms, unit_name, actor_id);
            const std::vector<std::string> clauses = on_split(terms_x, ';');
            for (std::size_t c = 0; c < clauses.size(); ++c) {
                const std::string clause = on_trim(clauses[c]);
                if (c == 0) {
                    const std::vector<std::string> f = on_split(clause, '.');
                    if (!f.empty()) r.head = f[0];
                    if (f.size() > 1) r.subject = f[1];
                    for (std::size_t i = 2; i < f.size(); ++i) {
                        if (i > 2) r.sub += '.';
                        r.sub += f[i];
                    }
                    continue;
                }
                if (clause.empty()) continue;
                const std::size_t sp = clause.find_first_of(" \t");
                if (sp == std::string::npos)
                    r.filters.emplace_back(clause, std::string{});
                else
                    r.filters.emplace_back(clause.substr(0, sp), on_trim(clause.substr(sp + 1)));
            }
        }
        if (send) {
            std::size_t i = 0;
            const std::string v = expand_actor_tokens(*send, unit_name, actor_id);
            while (i < v.size()) {
                while (i < v.size() && (v[i] == ' ' || v[i] == '\t')) ++i;
                const std::size_t start = i;
                while (i < v.size() && v[i] != ' ' && v[i] != '\t') ++i;
                if (i > start) {
                    std::string tok = v.substr(start, i - start);
                    if (r.verb.empty() && r.args.empty())
                        r.verb = std::move(tok);
                    else
                        r.args.push_back(std::move(tok));
                }
            }
        }
        out.push_back(std::move(r));
    }
    return out;
}

const std::vector<Catalog::ActorSubscription>& Catalog::actors_on(const std::string& head, const std::string& subject,
                                                                  const std::string& sub) const {
    static const std::vector<ActorSubscription> kNone;
    if (!actors_on_built_) {
        actors_on_built_ = true;
        std::vector<std::string> fams;
        for (const auto& [name, idmap] : families_)
            if (name.rfind("CActor", 0) == 0) fams.push_back(name);
        std::sort(fams.begin(), fams.end());
        for (const std::string& fam : fams)
            for (const std::string& id : ids(fam)) {
                const Entry* e = find(fam, id);
                if (!e || e->is_default) continue;
                bool cand = false;
                for (const XmlNode& n : resolved(fam, id)) {
                    if (n.name == "Macros") cand = true;
                    if (n.name == "On")
                        if (const std::string* s = n.attr("Send"); s && on_trim(*s) == "Create") cand = true;
                    if (cand) break;
                }
                if (!cand) continue;
                for (const ActorRule& r : actor_rules(id, fam)) {
                    if (r.verb != "Create" || !r.args.empty() || !r.target.empty() || r.head.empty()) continue;
                    actors_on_[r.head + "." + r.subject + "." + r.sub].push_back({id, fam, r.filters});
                }
            }
    }
    auto it = actors_on_.find(head + "." + subject + "." + sub);
    return it == actors_on_.end() ? kNone : it->second;
}

std::string Catalog::ability_point_marker(const std::string& abil_id) const {
    std::vector<std::string> todo, seen;
    for (const XmlNode& n : resolved("CAbil", abil_id))
        if (n.name == "Effect")
            if (const std::string* v = n.attr("value"); v && !v->empty()) todo.push_back(*v);
    std::function<void(const XmlNode&)> scan = [&](const XmlNode& n) {
        for (const auto& kv : n.attrs)
            if (!kv.second.empty() && kv.second.find(',') == std::string::npos && find("CEffect", kv.second)) todo.push_back(kv.second);
        for (const XmlNode& c : n.children) scan(c);
    };
    std::string fallback;
    while (!todo.empty() && seen.size() < 64) {
        const std::string e = todo.back();
        todo.pop_back();
        if (std::find(seen.begin(), seen.end(), e) != seen.end() || !find("CEffect", e)) continue;
        seen.push_back(e);
        for (const ActorSubscription& sub : actors_on("Effect", e, "Start")) {
            if (sub.family != "CActorModel") continue;
            const auto model = scalar("CActorModel", sub.actor, "Model");
            if (!model || model->empty() || model->find("##") != std::string::npos) continue;
            bool on_point = false;
            for (const XmlNode& n : resolved("CActorModel", sub.actor))
                if (n.name == "HostSiteOps" && n.attr_or("Ops", "").find("SOpTargetPoint") != std::string::npos) on_point = true;
            if (on_point) return sub.actor;
            if (fallback.empty()) fallback = sub.actor;
        }
        for (const XmlNode& n : resolved("CEffect", e)) scan(n);
    }
    return fallback;
}

std::optional<Catalog::MorphTransition> Catalog::morph_transition(const std::string& to_unit) const {
    if (to_unit.empty()) return std::nullopt;
    ensure_actor_by_unit();
    const auto own = actor_by_unit_.find(to_unit);
    for (const ActorSubscription& s : actors_on("AbilMorph", "*", "Start")) {
        if (s.family != "CActorUnit" || (own != actor_by_unit_.end() && own->second == s.actor)) continue;
        bool to = false;
        for (const auto& f : s.filters)
            if (f.first == "MorphTo" && f.second == to_unit) to = true;
        if (!to) continue;
        const std::optional<std::string> model = scalar("CActorUnit", s.actor, "Model");
        if (!model || model->empty() || model->find("##") != std::string::npos) continue;
        MorphTransition t;
        t.actor = s.actor;
        t.model = *model;
        for (const ActorRule& r : actor_rules(s.actor)) {
            if (r.head != "AbilMorph" || r.sub != "Start" || r.verb != "AnimPlay" || r.args.size() < 2) continue;
            bool rto = false;
            for (const auto& f : r.filters)
                if (f.first == "MorphTo" && f.second == to_unit) rto = true;
            if (!rto) continue;
            t.slot = r.args[0];
            std::size_t p = 0;
            const std::string& a = r.args[1];
            while (p <= a.size()) {
                const std::size_t e = a.find(',', p);
                const std::string tok = a.substr(p, e == std::string::npos ? std::string::npos : e - p);
                if (!tok.empty()) t.props.push_back(tok);
                if (e == std::string::npos) break;
                p = e + 1;
            }
            for (std::size_t k = 2; k < r.args.size(); ++k)
                if (r.args[k].find("PlayForever") != std::string::npos) t.play_forever = true;
            break;
        }
        if (t.props.empty()) continue;
        return t;
    }
    return std::nullopt;
}

std::pair<double, double> Catalog::morph_actor_section(const std::string& abil_id) const {
    for (const XmlNode& info : resolved("CAbil", abil_id)) {
        if (info.name != "InfoArray") continue;
        for (const XmlNode& sec : info.children) {
            if (sec.name != "SectionArray" || sec.attr_or("index", "") != "Actor") continue;
            double delay = 0.0, dur = 0.0;
            for (const XmlNode& d : sec.children) {
                if (d.name != "DurationArray") continue;
                const std::string* v = d.attr("value");
                const double val = v ? std::atof(v->c_str()) : 0.0;
                const std::string idx = d.attr_or("index", "");
                if (idx == "Delay") delay = val;
                else if (idx == "Duration") dur = val;
            }
            return {delay, dur};
        }
    }
    return {0.0, 0.0};
}

std::array<float, 3> Catalog::unit_render_scale(const std::string& unit_id) const {
    const std::array<float, 3> kIdentity{1.0f, 1.0f, 1.0f};
    if (unit_id.empty()) return kIdentity;

    ensure_actor_by_unit();
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

void Catalog::ensure_actor_by_unit() const {
    if (actor_by_unit_built_) return;
    actor_by_unit_built_ = true;
    for (const std::string& aid : ids("CActorUnit")) {
        const Entry* e = find("CActorUnit", aid);
        if (!e) continue;
        const std::string* un = e->attr("unitName");
        const std::string& key = (un && !un->empty()) ? *un : aid;
        actor_by_unit_.emplace(key, aid);
    }
}

std::optional<std::array<float, 3>> Catalog::actor_tint(const std::string& unit_id, const std::string& terms_prefix,
                                                        const std::string& terms_contains) const {
    if (unit_id.empty()) return std::nullopt;
    ensure_actor_by_unit();
    auto it = actor_by_unit_.find(unit_id);
    if (it == actor_by_unit_.end()) return std::nullopt;
    std::optional<std::array<float, 3>> out;
    for (const XmlNode& n : resolved("CActorUnit", it->second)) {
        if (n.name != "On") continue;
        const std::string terms = n.attr_or("Terms", "");
        const std::string send = n.attr_or("Send", "");
        if (terms.rfind(terms_prefix, 0) != 0 || terms.find(terms_contains) == std::string::npos) continue;
        if (send.rfind("SetTintColor ", 0) != 0) continue;
        int r = 255, g = 255, b = 255;
        if (std::sscanf(send.c_str() + 13, "%d,%d,%d", &r, &g, &b) != 3) continue;
        out = std::array<float, 3>{float(r) / 255.0f, float(g) / 255.0f, float(b) / 255.0f};
    }
    return out;
}

void Catalog::build_range_rings() const {
    if (range_rings_built_) return;
    range_rings_built_ = true;

    auto in_placement_chain = [&](const std::string& actor_id) {
        std::string cur = actor_id;
        for (int hops = 0; hops < 16 && !cur.empty(); ++hops) {
            if (cur == "PlacementModel") return true;
            const Entry* e = find("CActor", cur);
            if (!e) return false;
            cur = e->parent;
        }
        return false;
    };
    auto field = [&](const std::string& id, const char* name, const char* key) -> std::string {
        const auto& kids = resolved("CActor", id);
        std::string v;
        for (const XmlNode& n : kids)
            if (n.name == name)
                if (const std::string* a = n.attr(key)) v = *a;
        if (v.empty()) return v;
        v = expand_entry_tokens("CActor", id, v);
        return v.find("##") == std::string::npos ? v : std::string();
    };

    std::unordered_set<std::string> placement_rings;
    const std::vector<std::string> actors = ids("CActor");
    for (const std::string& id : actors) {
        const Entry* e = find("CActor", id);
        if (!e) continue;
        const bool placement_parent = in_placement_chain(id);
        for (const XmlNode& n : e->own) {
            if (n.name != "On") continue;
            const std::string terms = n.attr_or("Terms", "");
            const std::string send = n.attr_or("Send", "");
            if (terms.rfind("ActorCreation", 0) != 0 || send.rfind("Create ", 0) != 0) continue;
            if (!placement_parent && terms.find("Placement") == std::string::npos) continue;
            const std::string ring = send.substr(7);
            const Entry* r = find("CActor", ring);
            if (r && r->tag == "CActorRange") placement_rings.insert(ring);
        }
    }

    for (const std::string& id : actors) {
        const Entry* e = find("CActor", id);
        if (!e || e->tag != "CActorRange") continue;
        const std::string weapon = field(id, "Weapon", "value");
        const std::string abil = field(id, "Abil", "Link");
        const std::string sight = field(id, "Sight", "value");
        const std::string own_range = field(id, "Range", "value");
        double radius = 0;
        if (!own_range.empty()) radius = std::strtod(own_range.c_str(), nullptr);
        else if (!weapon.empty()) radius = std::strtod(scalar("CWeapon", weapon, "Range").value_or("0").c_str(), nullptr);
        else if (!abil.empty()) radius = std::strtod(scalar("CAbil", abil, "Range").value_or("0").c_str(), nullptr);
        else if (!sight.empty()) radius = std::strtod(scalar("CUnit", sight, "Sight").value_or("0").c_str(), nullptr);
        if (radius <= 0) continue;

        std::vector<std::string> selected_units;
        bool targeting = false;
        for (const XmlNode& n : resolved("CActor", id)) {
            if (n.name != "On") continue;
            const std::string terms = expand_entry_tokens("CActor", id, n.attr_or("Terms", ""));
            const std::string send = n.attr_or("Send", "");
            if (terms.rfind("SelectionLocalUpdate.", 0) == 0 && (send == "Create" || send == "SetVisibility 1")) {
                const std::size_t a = 21, b = terms.find('.', a);
                if (b != std::string::npos && terms.compare(b, 6, ".Start") == 0) {
                    const std::string unit = terms.substr(a, b - a);
                    if (unit != "*") selected_units.push_back(unit);
                }
            }
            if (terms.rfind("Abil.", 0) == 0 && terms.size() > 14 &&
                terms.compare(terms.size() - 9, 9, ".TargetOn") == 0 && send == "Create")
                targeting = true;
        }
        if (targeting && !abil.empty()) abil_rings_[abil] = radius;

        RangeRing ring;
        ring.radius = radius;
        ring.source = id;
        const bool placed = placement_rings.count(id) != 0;
        for (const std::string& unit : selected_units) {
            RangeRing r = ring;
            r.on_selection = true;
            range_rings_[unit].push_back(r);
        }
        if (placed) {
            for (const std::string& unit : ids("CUnit")) {
                bool owns = false;
                if (!weapon.empty()) {
                    for (const std::string& w : equipped_weapons(unit, true))
                        if (w == weapon) { owns = true; break; }
                } else if (!abil.empty()) {
                    owns = unit_carries_ability(unit, abil);
                }
                if (!owns) continue;
                std::vector<RangeRing>& rs = range_rings_[unit];
                bool merged = false;
                for (RangeRing& r : rs)
                    if (r.source == id) { r.on_placement = true; merged = true; }
                if (!merged) {
                    RangeRing r = ring;
                    r.on_placement = true;
                    rs.push_back(r);
                }
            }
        }
    }
}

}
