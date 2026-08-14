#include <algorithm>
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
    std::vector<TwoSlot> slots = resolve_array_slots(family, id, field);
    std::vector<std::string> out;
    for (const TwoSlot& s : slots)
        if (s.present && !s.dead && !s.link.empty()) out.push_back(s.link);
    return out;
}

std::vector<std::string> Catalog::array_links_legacy(const std::string& family,
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

std::vector<Catalog::TwoSlot> Catalog::resolve_array_slots(const std::string& family,
                                                           const std::string& id,
                                                           const std::string& field) const {
    const Entry* e = find(family, id);
    if (!e) return {};

    std::vector<TwoSlot> base;
    if (!e->parent.empty() && e->parent != id) base = resolve_array_slots(family, e->parent, field);

    struct Own { long key; bool is_pos; std::string link; bool present; bool dead; };
    std::vector<Own> own;
    std::unordered_map<long, size_t> own_at;
    long counter = 0;
    long cur_decl = -1;
    for (size_t k = 0; k < e->own.size(); ++k) {
        if (e->own[k].name != field) {
            if (k < e->own_decl.size() && long(e->own_decl[k]) != cur_decl) {
                cur_decl = long(e->own_decl[k]); counter = 0;
            }
            continue;
        }
        if (k < e->own_decl.size() && long(e->own_decl[k]) != cur_decl) {
            cur_decl = long(e->own_decl[k]); counter = 0;
        }
        const XmlNode& c = e->own[k];
        const std::string* lv = c.attr("Link");
        if (!lv) lv = c.attr("value");
        const bool has_link = lv != nullptr;
        std::string link = has_link ? expand_id(*lv, id) : std::string{};
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
        if (has_index) { key = index; is_pos = false; counter = index + 1; }
        else { key = counter; is_pos = true; ++counter; }
        auto it = own_at.find(key);
        if (it == own_at.end()) {
            own_at[key] = own.size();
            own.push_back(Own{key, is_pos, link, has_link, rm_state == 1});
        } else {
            Own& o = own[it->second];
            if (has_link) { o.link = link; o.present = true; }
            if (rm_state == 1) o.dead = true;
            else if (rm_state == 0) o.dead = false;
        }
    }

    for (const Own& o : own) {
        if (o.is_pos) {
            base.push_back(TwoSlot{o.link, o.present, o.dead});
        } else {
            size_t idx = size_t(o.key < 0 ? 0 : o.key);
            if (idx >= base.size()) base.resize(idx + 1);
            if (o.present) { base[idx].link = o.link; base[idx].present = true; }
            if (o.dead) base[idx].dead = true;
        }
    }
    return base;
}

std::vector<std::string> Catalog::array_links_twomode(const std::string& family,
                                                      const std::string& id,
                                                      const std::string& field) const {
    return array_links(family, id, field);
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

std::optional<int> Catalog::effect_damage(const std::string& effect_id, int depth,
                                          const std::string& prefer) const {
    return walk_damage_tree<int>(
        effect_id, depth,
        [this](const std::string& id) -> std::optional<int> {
            if (auto a = scalar_num("CEffect", id, "Amount")) return int(*a);
            return std::nullopt;
        },
        [this](int sub, const std::string& id) -> int {
            const int pc = int(scalar_num("CEffect", id, "PeriodCount").value_or(1));
            return sub * (pc > 0 ? pc : 1);
        },
        prefer);
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
    const std::string eff = weapon_effect_id(weapon_id);
    const std::string disp = scalar("CWeapon", weapon_id, "DisplayEffect").value_or("");
    if (!disp.empty())
        if (auto d = effect_damage(eff, 0, disp)) return d;
    if (auto d = effect_damage(eff, 0)) return d;
    if (!disp.empty())
        return effect_damage(disp, 0);
    return std::nullopt;
}

std::optional<int> Catalog::effect_hit_count(const std::string& effect_id, int depth,
                                             const std::string& prefer) const {
    return walk_damage_tree<int>(
        effect_id, depth,
        [](const std::string&) -> std::optional<int> { return 1; },
        [this](int sub, const std::string& id) -> int {
            const int pc = int(scalar_num("CEffect", id, "PeriodCount").value_or(1));
            return sub * (pc > 0 ? pc : 1);
        },
        prefer);
}

std::optional<int> Catalog::weapon_hit_count(const std::string& weapon_id) const {
    const Entry* w = find("CWeapon", weapon_id);
    if (!w) return std::nullopt;
    const std::string eff = weapon_effect_id(weapon_id);
    const std::string disp = scalar("CWeapon", weapon_id, "DisplayEffect").value_or("");
    if (!disp.empty())
        if (auto c = effect_hit_count(eff, 0, disp)) return c;
    if (auto c = effect_hit_count(eff, 0)) return c;
    if (!disp.empty())
        return effect_hit_count(disp, 0);
    return std::nullopt;
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

std::vector<std::pair<std::string, std::string>>
Catalog::unit_ability_commands(const std::string& unit_id) const {
    std::vector<std::pair<std::string, std::string>> out;
    for (const core::XmlNode& n : resolved("CUnit", unit_id)) {
        if (n.name != "CardLayouts") continue;
        for (const core::XmlNode& b : n.children) {
            if (b.name != "LayoutButtons") continue;
            if (b.attr_or("Type", "") != "AbilCmd") continue;
            const std::string ac = b.attr_or("AbilCmd", "");
            if (ac.empty()) continue;
            const std::size_t comma = ac.find(',');
            std::string abil = comma == std::string::npos ? ac : ac.substr(0, comma);
            std::string cmd = comma == std::string::npos ? std::string() : ac.substr(comma + 1);
            if (abil.empty()) continue;
            bool dup = false;
            for (const auto& p : out)
                if (p.first == abil && p.second == cmd) { dup = true; break; }
            if (!dup) out.emplace_back(std::move(abil), std::move(cmd));
        }
    }
    return out;
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
    auto parse_int = [](const std::string& s) -> int {
        if (s.empty()) return -1;
        char* end = nullptr;
        long v = std::strtol(s.c_str(), &end, 10);
        return (end != s.c_str() && *end == '\0') ? static_cast<int>(v) : -1;
    };
    std::vector<AbilityCardButton> out;
    for (const core::XmlNode& n : resolved("CUnit", unit_id)) {
        if (n.name != "CardLayouts") continue;
        for (const core::XmlNode& b : n.children) {
            if (b.name != "LayoutButtons") continue;
            if (b.attr_or("Type", "") != "AbilCmd") continue;
            const std::string ac = b.attr_or("AbilCmd", "");
            if (ac.empty()) continue;
            const std::size_t comma = ac.find(',');
            std::string abil = comma == std::string::npos ? ac : ac.substr(0, comma);
            std::string cmd = comma == std::string::npos ? std::string() : ac.substr(comma + 1);
            if (abil.empty()) continue;
            const std::string face = b.attr_or("Face", "");
            const int row = parse_int(b.attr_or("Row", ""));
            const int col = parse_int(b.attr_or("Column", ""));
            AbilityCardButton* found = nullptr;
            for (AbilityCardButton& e : out)
                if (e.ability == abil && e.command == cmd) { found = &e; break; }
            if (found) {
                if (!face.empty()) found->face = face;
                if (row >= 0) found->row = row;
                if (col >= 0) found->col = col;
            } else {
                AbilityCardButton e;
                e.ability = std::move(abil);
                e.command = std::move(cmd);
                e.face = face;
                e.row = row;
                e.col = col;
                out.push_back(std::move(e));
            }
        }
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
                       "FootprintData.xml", "ButtonData.xml", "RequirementData.xml",
                       "RequirementNodeData.xml", "ActorData.xml",
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

}
