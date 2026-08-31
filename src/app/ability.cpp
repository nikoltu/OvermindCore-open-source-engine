#include "sc2/app/ability.hpp"

#include <algorithm>
#include <functional>
#include <utility>

#include "sc2/app/spawn.hpp"
#include "sc2/core/xml.hpp"
#include "sc2/data/economy.hpp"
#include "sc2/sim/weapon.hpp"

namespace sc2::app {

using sim::Fixed;

static std::uint32_t filter_attr_bit(const std::string& tok) {
    if (tok == "Light") return sim::kAttrLight;
    if (tok == "Armored") return sim::kAttrArmored;
    if (tok == "Biological") return sim::kAttrBiological;
    if (tok == "Mechanical") return sim::kAttrMechanical;
    if (tok == "Massive") return sim::kAttrMassive;
    if (tok == "Psionic") return sim::kAttrPsionic;
    if (tok == "Structure") return sim::kAttrStructure;
    return 0;
}

static void parse_ability_filters(const data::Catalog& cat, const std::string& abil_id,
                                  AbilityBuild& b) {
    std::string filters;
    for (const core::XmlNode& c : cat.resolved("CAbil", abil_id))
        if (c.name == "TargetFilters")
            if (const std::string* v = c.attr("value"); v && !v->empty()) filters = *v;
    if (filters.empty()) return;
    const std::size_t semi = filters.find(';');
    const std::string req = filters.substr(0, semi);
    const std::string exc = semi == std::string::npos ? "" : filters.substr(semi + 1);
    auto each = [](const std::string& list, const std::function<void(const std::string&)>& fn) {
        std::size_t s = 0;
        while (s <= list.size()) {
            std::size_t c = list.find(',', s);
            if (c == std::string::npos) c = list.size();
            if (c > s) fn(list.substr(s, c - s));
            s = c + 1;
        }
    };
    each(req, [&](const std::string& t) {
        if (t == "HasEnergy") b.require_energy = true;
        else if (t == "Ground") b.requires_ground = true;
        else b.req_attr_mask |= filter_attr_bit(t);
    });
    each(exc, [&](const std::string& t) { b.exc_attr_mask |= filter_attr_bit(t); });
}

static Fixed cabil_energy(const data::Catalog& cat, const std::string& id) {
    Fixed result;
    for (const core::XmlNode& c : cat.resolved("CAbil", id)) {
        if (c.name != "Cost") continue;
        std::string v = c.attr_or("Energy", "");
        if (v.empty())
            for (const core::XmlNode& g : c.children)
                if (g.name == "Vital" && g.attr_or("index", "") == "Energy")
                    v = g.attr_or("value", "");
        if (!v.empty()) result = Fixed::parse(v);
    }
    return result;
}

static Fixed cabil_life(const data::Catalog& cat, const std::string& id) {
    Fixed result;
    for (const core::XmlNode& c : cat.resolved("CAbil", id)) {
        if (c.name != "Cost") continue;
        for (const core::XmlNode& g : c.children)
            if (g.name == "Vital" && g.attr_or("index", "") == "Life") {
                const std::string v = g.attr_or("value", "");
                if (!v.empty()) result = Fixed::parse(v);
            }
    }
    return result;
}

static Fixed cabil_cooldown(const data::Catalog& cat, const std::string& id) {
    Fixed result;
    for (const core::XmlNode& c : cat.resolved("CAbil", id)) {
        if (c.name != "Cost") continue;
        for (const core::XmlNode& cd : c.children) {
            if (cd.name != "Cooldown") continue;
            std::string v = cd.attr_or("TimeUse", "");
            if (v.empty())
                for (const core::XmlNode& t : cd.children)
                    if (t.name == "TimeUse") v = t.attr_or("value", "");
            if (!v.empty()) result = Fixed::parse(v);
        }
    }
    return result;
}

Fixed ability_energy_cost(const data::Catalog& cat, const std::string& abil_id) {
    return cabil_energy(cat, abil_id);
}
Fixed ability_cooldown_sec(const data::Catalog& cat, const std::string& abil_id) {
    return cabil_cooldown(cat, abil_id);
}

AbilTargetMode ability_target_mode(const data::Catalog& cat, const std::string& abil_id) {
    if (const auto* e = cat.find("CAbil", abil_id); e && e->tag == "CAbilEffectInstant")
        return AbilTargetMode::Instant;
    auto b = make_ability(cat, abil_id);
    if (!b) return AbilTargetMode::Unit;
    const bool has_tree =
        !b->tree.empty() && b->tree_root >= 0 && b->tree_root < std::int32_t(b->tree.size());
    const sim::EffectKind kind = has_tree ? b->tree[b->tree_root].kind : b->def.kind;
    switch (kind) {
        case sim::EffectKind::CreatePersistent:
        case sim::EffectKind::EnumArea:
        case sim::EffectKind::CreateUnit:
        case sim::EffectKind::Teleport:
        case sim::EffectKind::Reveal:
        case sim::EffectKind::PhaseShift:
            return AbilTargetMode::Point;
        default:
            return AbilTargetMode::Unit;
    }
}

static Fixed cabil_cast_phase(const data::Catalog& cat, const std::string& id, const char* field) {
    Fixed result;
    for (const core::XmlNode& c : cat.resolved("CAbil", id)) {
        if (c.name != field) continue;
        if (const std::string* ix = c.attr("index"); ix && *ix != "0") continue;
        if (const std::string* v = c.attr("value"); v && !v->empty()) result = Fixed::parse(*v);
    }
    return result;
}

static void bind_cast_times(const data::Catalog& cat, const std::string& abil_id, AbilityBuild& b) {
    b.cast_intro_time = cabil_cast_phase(cat, abil_id, "CastIntroTime");
    b.cast_finish_time = cabil_cast_phase(cat, abil_id, "FinishTime");
    b.cast_outro_time = cabil_cast_phase(cat, abil_id, "CastOutroTime");
}

double weapon_upgrade_bonus(const data::Catalog& cat, const data::UpgradeCatalog& up,
                            const std::string& weapon_id,
                            const std::vector<std::string>& researched) {
    const auto eff = cat.weapon_damage_effect_id(weapon_id);
    if (!eff) return 0.0;
    double bonus = 0.0;
    for (const std::string& upg : researched) {
        if (!up.contains(upg)) continue;
        for (const data::UpgradeEffect& e : up.resolve(upg))
            if (e.catalog == "Effect" && e.entry == *eff && e.field == "Amount")
                bonus += e.delta();
    }
    return bonus;
}

std::vector<std::string> army_weapon_upgrade_ids(const data::Catalog& cat, const data::UpgradeCatalog& up,
                                                 const std::string& army_unit) {
    std::vector<std::string> out;
    if (army_unit.empty()) return out;
    std::string eff;
    for (const std::string& w : cat.array_links("CUnit", army_unit, "WeaponArray"))
        if (!w.empty() && cat.weapon_damage(w))
            if (auto d = cat.weapon_damage_effect_id(w)) {
                eff = *d;
                break;
            }
    if (eff.empty()) return out;
    for (const std::string& id : up.ids()) {
        if (id.empty() || !std::isdigit(static_cast<unsigned char>(id.back())))
            continue;
        for (const data::UpgradeEffect& e : up.resolve(id))
            if (e.catalog == "Effect" && e.entry == eff && e.field == "Amount") {
                out.push_back(id);
                break;
            }
    }
    std::stable_sort(out.begin(), out.end(),
                     [](const std::string& a, const std::string& b) { return a.back() < b.back(); });
    return out;
}

std::vector<std::string> army_armor_upgrade_ids(const data::UpgradeCatalog& up,
                                                const std::string& army_unit) {
    std::vector<std::string> out;
    if (army_unit.empty()) return out;
    for (const std::string& id : up.ids()) {
        if (id.empty() || !std::isdigit(static_cast<unsigned char>(id.back())))
            continue;
        for (const data::UpgradeEffect& e : up.resolve(id))
            if (e.catalog == "Unit" && e.entry == army_unit && e.field == "LifeArmor") {
                out.push_back(id);
                break;
            }
    }
    std::stable_sort(out.begin(), out.end(),
                     [](const std::string& a, const std::string& b) { return a.back() < b.back(); });
    return out;
}

std::vector<std::string> army_shield_upgrade_ids(const data::UpgradeCatalog& up,
                                                 const std::string& army_unit) {
    std::vector<std::string> out;
    if (army_unit.empty()) return out;
    for (const std::string& id : up.ids()) {
        if (id.empty() || !std::isdigit(static_cast<unsigned char>(id.back())))
            continue;
        for (const data::UpgradeEffect& e : up.resolve(id))
            if (e.catalog == "Unit" && e.entry == army_unit && e.field == "ShieldArmor") {
                out.push_back(id);
                break;
            }
    }
    std::stable_sort(out.begin(), out.end(),
                     [](const std::string& a, const std::string& b) { return a.back() < b.back(); });
    return out;
}

double armor_upgrade_bonus(const data::UpgradeCatalog& up, const std::string& unit_name,
                           const std::vector<std::string>& researched) {
    double bonus = 0.0;
    for (const std::string& upg : researched) {
        if (!up.contains(upg)) continue;
        for (const data::UpgradeEffect& e : up.resolve(upg))
            if (e.catalog == "Unit" && e.entry == unit_name && e.field == "LifeArmor")
                bonus += e.delta();
    }
    return bonus;
}

static double fold_field(const data::UpgradeCatalog& up, const std::vector<std::string>& researched,
                         const char* catalog, const std::string& entry, const char* field, double base) {
    double v = base;
    for (const std::string& upg : researched) {
        if (!up.contains(upg)) continue;
        for (const data::UpgradeEffect& e : up.resolve(upg))
            if (e.catalog == catalog && e.entry == entry && e.field == field)
                v = e.apply(v);
    }
    return v;
}

void apply_unit_upgrades(const data::Catalog& cat, const data::UpgradeCatalog& up,
                         const std::string& unit_name, const std::vector<std::string>& researched,
                         sim::Unit& u) {
    if (researched.empty()) return;
    std::vector<std::string> weapons;
    for (const std::string& w : cat.equipped_weapons(unit_name))
        if (cat.weapon_damage(w)) weapons.push_back(w);

    auto fold_weapon = [&](const std::string& w, Fixed& dst) {
        auto eff = cat.weapon_damage_effect_id(w);
        if (!eff) return;
        const int h = cat.weapon_hit_count(w).value_or(1);
        const double per_hit = dst.to_double() / (h > 0 ? h : 1);
        dst = Fixed::from_double(fold_field(up, researched, "Effect", *eff, "Amount", per_hit) *
                                 (h > 0 ? h : 1));
    };
    if (!weapons.empty()) fold_weapon(weapons[0], u.wpn_damage);
    if (weapons.size() >= 2) fold_weapon(weapons[1], u.wpn2_damage);

    if (!weapons.empty())
        weapon_attribute_bonus(cat, weapons[0], u.wpn_bonus_attr, u.wpn_bonus_amount, &up,
                               &researched);
    if (weapons.size() >= 2)
        weapon_attribute_bonus(cat, weapons[1], u.wpn2_bonus_attr, u.wpn2_bonus_amount, &up,
                               &researched);

    u.armor = Fixed::from_double(fold_field(up, researched, "Unit", unit_name, "LifeArmor",
                                            u.armor.to_double()));

    u.speed        = Fixed::from_double(fold_field(up, researched, "Unit", unit_name, "Speed",
                                                   u.speed.to_double()));
    u.shield_armor = Fixed::from_double(fold_field(up, researched, "Unit", unit_name, "ShieldArmor",
                                                   u.shield_armor.to_double()));
    const double base_life_max = u.hp_max.to_double();
    const double new_life_max = fold_field(up, researched, "Unit", unit_name, "LifeMax", base_life_max);
    if (new_life_max != base_life_max) {
        u.hp_max = Fixed::from_double(new_life_max);
        u.hp = u.hp + Fixed::from_double(new_life_max - base_life_max);
    }
    u.energy_max = Fixed::from_double(fold_field(up, researched, "Unit", unit_name, "EnergyMax",
                                                 u.energy_max.to_double()));
    u.energy     = Fixed::from_double(fold_field(up, researched, "Unit", unit_name, "Energy",
                                                 u.energy.to_double()));
    if (!weapons.empty())
        u.wpn_range = Fixed::from_double(fold_field(up, researched, "Weapon", weapons[0], "Range",
                                                    u.wpn_range.to_double()));
    if (weapons.size() >= 2)
        u.wpn2_range = Fixed::from_double(fold_field(up, researched, "Weapon", weapons[1], "Range",
                                                     u.wpn2_range.to_double()));
    double rate[2] = {0.0, 0.0};
    for (const std::string& upg : researched) {
        if (!up.contains(upg)) continue;
        for (const data::UpgradeEffect& e : up.resolve(upg)) {
            if (e.catalog != "Weapon" || e.field != "RateMultiplier") continue;
            int idx = -1;
            if (!weapons.empty() && weapons[0] == e.entry) idx = 0;
            else if (weapons.size() >= 2 && weapons[1] == e.entry) idx = 1;
            if (idx >= 0) rate[idx] = e.delta();
        }
    }
    if (rate[0] != 0.0 && u.wpn_period.raw() > 0)
        u.wpn_period = u.wpn_period / Fixed::from_double(1.0 + rate[0]);
    if (rate[1] != 0.0 && u.wpn2_period.raw() > 0)
        u.wpn2_period = u.wpn2_period / Fixed::from_double(1.0 + rate[1]);
}

static UpgradeRevert snapshot_revert(std::uint32_t id, const sim::Unit& u) {
    return UpgradeRevert{id, u.wpn_damage, u.wpn2_damage, u.armor, u.speed, u.shield_armor,
                         u.hp_max, u.hp, u.energy_max, u.energy, u.wpn_range, u.wpn2_range,
                         u.wpn_period, u.wpn2_period};
}
static bool revert_differs(const UpgradeRevert& r, const sim::Unit& u) {
    return r.wpn_damage != u.wpn_damage || r.wpn2_damage != u.wpn2_damage || r.armor != u.armor ||
           r.speed != u.speed || r.shield_armor != u.shield_armor || r.hp_max != u.hp_max ||
           r.hp != u.hp || r.energy_max != u.energy_max || r.energy != u.energy ||
           r.wpn_range != u.wpn_range || r.wpn2_range != u.wpn2_range ||
           r.wpn_period != u.wpn_period || r.wpn2_period != u.wpn2_period;
}

std::size_t apply_upgrade_to_live(sim::World& w, const data::Catalog& cat,
                                  const data::UpgradeCatalog& up, const std::string& upgrade_id,
                                  int player, std::vector<UpgradeRevert>* revert) {
    if (!up.contains(upgrade_id)) return 0;
    const std::vector<std::string> just{upgrade_id};
    std::size_t pushed = 0;
    for (std::uint32_t id = 0; id < w.units().size(); ++id) {
        sim::Unit& u = w.unit(id);
        if (!u.alive || u.player != player || u.catalog_id.empty()) continue;
        const UpgradeRevert pre = snapshot_revert(id, u);
        apply_unit_upgrades(cat, up, u.catalog_id, just, u);
        if (revert && revert_differs(pre, u)) {
            revert->push_back(pre);
            ++pushed;
        }
    }
    return pushed;
}

void revert_live_upgrade(sim::World& w, std::vector<UpgradeRevert>& revert, std::size_t count) {
    for (std::size_t k = 0; k < count && !revert.empty(); ++k) {
        const UpgradeRevert r = revert.back();
        revert.pop_back();
        if (r.unit_id >= w.units().size()) continue;
        sim::Unit& u = w.unit(r.unit_id);
        u.wpn_damage = r.wpn_damage;
        u.wpn2_damage = r.wpn2_damage;
        u.armor = r.armor;
        u.speed = r.speed;
        u.shield_armor = r.shield_armor;
        u.hp_max = r.hp_max;
        u.hp = r.hp;
        u.energy_max = r.energy_max;
        u.energy = r.energy;
        u.wpn_range = r.wpn_range;
        u.wpn2_range = r.wpn2_range;
        u.wpn_period = r.wpn_period;
        u.wpn2_period = r.wpn2_period;
    }
}

static std::optional<sim::VitalGate> parse_vital_validator(const data::Catalog& cat,
                                                           const std::string& vid) {
    if (vid.empty()) return std::nullopt;
    const data::Catalog::Entry* ve = cat.find("CValidator", vid);
    if (!ve || ve->tag != "CValidatorUnitCompareVital") return std::nullopt;
    sim::VitalGate g;
    const std::string vital = cat.scalar("CValidator", vid, "Vital").value_or("Life");
    g.vital = vital == "Energy" ? sim::VitalGate::Energy
              : vital == "Shields" ? sim::VitalGate::Shields
                                   : sim::VitalGate::Life;
    const std::string cmp = cat.scalar("CValidator", vid, "Compare").value_or("LT");
    g.cmp = cmp == "LE"   ? sim::VitalGate::LE
            : cmp == "GT" ? sim::VitalGate::GT
            : cmp == "GE" ? sim::VitalGate::GE
            : cmp == "EQ" ? sim::VitalGate::EQ
            : cmp == "NE" ? sim::VitalGate::NE
                          : sim::VitalGate::LT;
    g.value = Fixed::parse(cat.scalar("CValidator", vid, "Value").value_or("0"));
    g.fraction = cat.scalar("CValidator", vid, "Type").value_or("") == "Fraction";
    return g;
}

static std::vector<sim::VitalGate> parse_effect_vital_gates(const data::Catalog& cat,
                                                            const std::string& eff) {
    std::vector<sim::VitalGate> gates;
    if (eff.empty()) return gates;
    for (const std::string& vid : cat.array_links("CEffect", eff, "ValidatorArray"))
        if (auto g = parse_vital_validator(cat, vid)) gates.push_back(*g);
    return gates;
}
static void parse_ability_validators(const data::Catalog& cat, const std::string& eff,
                                     AbilityBuild& b) {
    auto gates = parse_effect_vital_gates(cat, eff);
    b.vital_gates.insert(b.vital_gates.end(), gates.begin(), gates.end());
}

std::optional<AbilityBuild> make_ability(const data::Catalog& cat, const std::string& abil_id) {
    if (auto m = cat.morph_spec(abil_id)) {
        AbilityBuild b;
        b.def.kind = sim::EffectKind::Morph;
        b.def.duration = Fixed::from_double(m->morph_time);
        b.spawn_unit = m->target;
        if (!m->finish_effect.empty()) {
            if (auto lf = cat.effect_vital_fraction(m->finish_effect, "Life"); lf && *lf > 0)
                b.def.morph_heal_life_frac = Fixed::from_double(*lf);
            if (auto sf = cat.effect_vital_fraction(m->finish_effect, "Shields"); sf && *sf > 0)
                b.def.morph_heal_shield_frac = Fixed::from_double(*sf);
        }
        b.energy_cost = cabil_energy(cat, abil_id);
    b.life_cost = cabil_life(cat, abil_id);
        b.cooldown = cabil_cooldown(cat, abil_id);
        bind_cast_times(cat, abil_id, b);
        return b;
    }

    if (auto rv = cat.reveal_ability(abil_id); rv.found) {
        AbilityBuild b;
        b.def.kind = sim::EffectKind::Reveal;
        b.def.radius = Fixed::from_double(rv.search_radius > 0 ? rv.search_radius : rv.reveal_radius);
        b.def.duration = Fixed::from_double(rv.duration);
        if (auto r = cat.scalar("CAbil", abil_id, "Range"); r && !r->empty())
            b.def.range = Fixed::parse(*r);
        b.energy_cost = cabil_energy(cat, abil_id);
        b.life_cost = cabil_life(cat, abil_id);
        b.cooldown = cabil_cooldown(cat, abil_id);
        bind_cast_times(cat, abil_id, b);
        return b;
    }

    if (auto bs = cat.build_spec(abil_id)) {
        AbilityBuild b;
        b.def.kind = sim::EffectKind::CreateUnit;
        b.def.spawn_count = 1;
        b.spawn_unit = bs->unit;
        b.spawn_requires_creep = cat.creep_source_radius(bs->unit) > 0;
        b.energy_cost = Fixed::from_double(bs->energy);
        b.cooldown = cabil_cooldown(cat, abil_id);
        bind_cast_times(cat, abil_id, b);
        return b;
    }

    if (auto ps = cat.phase_shift_spec(abil_id); ps.found) {
        AbilityBuild b;
        b.def.kind = sim::EffectKind::PhaseShift;
        b.def.duration = Fixed::from_double(ps.duration);
        b.def.spawn_count = 1;
        b.spawn_unit = ps.shade;
        if (auto r = cat.scalar("CAbil", abil_id, "Range"); r && !r->empty())
            b.def.range = Fixed::parse(*r);
        b.energy_cost = cabil_energy(cat, abil_id);
        b.life_cost = cabil_life(cat, abil_id);
        b.cooldown = cabil_cooldown(cat, abil_id);
        bind_cast_times(cat, abil_id, b);
        return b;
    }

    if (auto iv = cat.inject_spec(abil_id); iv.found) {
        AbilityBuild b;
        b.def.kind = sim::EffectKind::Inject;
        b.def.duration = Fixed::from_double(iv.delay);
        b.def.spawn_count = iv.count;
        b.def.period = Fixed::from_double(iv.stun);
        if (auto r = cat.scalar("CAbil", abil_id, "Range"); r && !r->empty())
            b.def.range = Fixed::parse(*r);
        b.energy_cost = cabil_energy(cat, abil_id);
        b.life_cost = cabil_life(cat, abil_id);
        b.cooldown = cabil_cooldown(cat, abil_id);
        b.requires_spawner_target = true;
        bind_cast_times(cat, abil_id, b);
        return b;
    }

    if (auto mc = cat.mule_calldown_spec(abil_id); mc.found) {
        AbilityBuild b;
        b.def.kind = sim::EffectKind::CreateUnit;
        b.def.spawn_count = 1;
        b.def.spawn_snap_resource = true;
        b.spawn_unit = mc.unit;
        b.spawn_life = Fixed::from_double(mc.life);
        if (auto r = cat.scalar("CAbil", abil_id, "Range"); r && !r->empty())
            b.def.range = Fixed::parse(*r);
        b.energy_cost = cabil_energy(cat, abil_id);
        b.life_cost = cabil_life(cat, abil_id);
        b.cooldown = cabil_cooldown(cat, abil_id);
        bind_cast_times(cat, abil_id, b);
        return b;
    }

    if (auto it = cat.infested_terran_spec(abil_id); it.found && !it.unit.empty()) {
        AbilityBuild b;
        b.def.kind = sim::EffectKind::CreateUnit;
        b.def.spawn_count = it.count < 1 ? 1 : it.count;
        b.spawn_unit = it.unit;
        b.spawn_life = Fixed::from_double(it.life);
        if (auto r = cat.scalar("CAbil", abil_id, "Range"); r && !r->empty())
            b.def.range = Fixed::parse(*r);
        b.energy_cost = cabil_energy(cat, abil_id);
        b.life_cost = cabil_life(cat, abil_id);
        b.cooldown = cabil_cooldown(cat, abil_id);
        bind_cast_times(cat, abil_id, b);
        return b;
    }

    const std::string eff = cat.cabil_effect_link(abil_id);

    if (auto rc = cat.recall_spec(abil_id)) {
        AbilityBuild b;
        sim::EffectNode tp;
        tp.kind = sim::EffectKind::Teleport;
        tp.teleport_to_source = true;
        sim::EffectNode ea;
        ea.kind = sim::EffectKind::EnumArea;
        ea.radius = Fixed::from_double(rc->radius);
        ea.search_affiliation = cat.effect_search_affiliation(rc->search_effect);
        ea.search_exclude_structure =
            cat.effect_search_excludes_structure(rc->search_effect);
        ea.children = {0};
        b.tree = {tp, ea};
        b.tree_root = 1;
        b.def.kind = sim::EffectKind::EnumArea;
        if (auto r = cat.scalar("CAbil", abil_id, "Range"); r && !r->empty())
            b.def.range = Fixed::parse(*r);
        b.energy_cost = cabil_energy(cat, abil_id);
        b.life_cost = cabil_life(cat, abil_id);
        b.cooldown = cabil_cooldown(cat, abil_id);
        bind_cast_times(cat, abil_id, b);
        return b;
    }

    if (auto ctb = cat.effect_control_transfer_behavior(eff)) {
        if (auto bb = cat.behavior_buff(*ctb)) {
            AbilityBuild b;
            b.def.kind = sim::EffectKind::ApplyBehavior;
            b.def.duration = Fixed::from_double(bb->duration);
            b.def.beh_control_transfer = true;
            if (auto r = cat.scalar("CAbil", abil_id, "Range"); r && !r->empty())
                b.def.range = Fixed::parse(*r);
            b.energy_cost = cabil_energy(cat, abil_id);
            b.life_cost = cabil_life(cat, abil_id);
            b.cooldown = cabil_cooldown(cat, abil_id);
            bind_cast_times(cat, abil_id, b);
            parse_ability_filters(cat, abil_id, b);
            parse_ability_validators(cat, eff, b);
            return b;
        }
    }

    if (cat.effect_subclass(eff) == "LaunchMissile") {
        AbilityBuild missile;
        if (build_effect(cat, eff, missile) >= 0 &&
            missile.tree[missile.tree_root].kind == sim::EffectKind::EnumArea) {
            missile.def.kind = missile.tree[missile.tree_root].kind;
            if (auto r = cat.scalar("CAbil", abil_id, "Range"); r && !r->empty())
                missile.def.range = Fixed::parse(*r);
            missile.energy_cost = cabil_energy(cat, abil_id);
            missile.life_cost = cabil_life(cat, abil_id);
            missile.cooldown = cabil_cooldown(cat, abil_id);
            bind_cast_times(cat, abil_id, missile);
            parse_ability_filters(cat, abil_id, missile);
            parse_ability_validators(cat, eff, missile);
            return missile;
        }
    }
    if (cat.effect_subclass(eff) == "EnumArea") {
        AbilityBuild area;
        if (build_effect(cat, eff, area) >= 0) {
            area.def.kind = area.tree[area.tree_root].kind;
            if (auto r = cat.scalar("CAbil", abil_id, "Range"); r && !r->empty())
                area.def.range = Fixed::parse(*r);
            area.energy_cost = cabil_energy(cat, abil_id);
            area.life_cost = cabil_life(cat, abil_id);
            area.cooldown = cabil_cooldown(cat, abil_id);
            bind_cast_times(cat, abil_id, area);
            parse_ability_filters(cat, abil_id, area);
            parse_ability_validators(cat, eff, area);
            return area;
        }
    }
    if (cat.effect_subclass(eff) == "CreateHealer") {
        AbilityBuild heal;
        if (build_effect(cat, eff, heal) >= 0) {
            heal.def.kind = heal.tree[heal.tree_root].kind;
            if (auto r = cat.scalar("CAbil", abil_id, "Range"); r && !r->empty())
                heal.def.range = Fixed::parse(*r);
            heal.energy_cost = cabil_energy(cat, abil_id);
            heal.life_cost = cabil_life(cat, abil_id);
            heal.cooldown = cabil_cooldown(cat, abil_id);
            bind_cast_times(cat, abil_id, heal);
            parse_ability_filters(cat, abil_id, heal);
            return heal;
        }
    }

    if (cat.effect_subclass(eff) == "CreatePersistent" && cat.effect_flag_set(eff, "Channeled") &&
        cat.persistent_applies_stun(eff)) {
        auto pc = cat.scalar_num("CEffect", eff, "PeriodCount");
        auto pp = cat.scalar_num("CEffect", eff, "PeriodicPeriodArray");
        if (pc && pp && *pc > 0 && *pp > 0) {
            AbilityBuild lift;
            lift.def.kind = sim::EffectKind::ApplyBehavior;
            lift.def.duration = Fixed::from_double((*pc) * (*pp));
            lift.def.beh_stun = true;
            lift.def.beh_lift = cat.persistent_applies_airlift(eff);
            if (auto r = cat.scalar("CAbil", abil_id, "Range"); r && !r->empty())
                lift.def.range = Fixed::parse(*r);
            lift.energy_cost = cabil_energy(cat, abil_id);
            lift.life_cost = cabil_life(cat, abil_id);
            lift.cooldown = cabil_cooldown(cat, abil_id);
            bind_cast_times(cat, abil_id, lift);
            parse_ability_filters(cat, abil_id, lift);
            return lift;
        }
    }

    if (auto pull = cat.abduct_pull_time(abil_id)) {
        AbilityBuild yank;
        yank.def.kind = sim::EffectKind::Force;
        yank.def.duration = Fixed::from_double(*pull);
        if (auto r = cat.scalar("CAbil", abil_id, "Range"); r && !r->empty())
            yank.def.range = Fixed::parse(*r);
        yank.energy_cost = cabil_energy(cat, abil_id);
        yank.life_cost = cabil_life(cat, abil_id);
        yank.cooldown = cabil_cooldown(cat, abil_id);
        bind_cast_times(cat, abil_id, yank);
        parse_ability_filters(cat, abil_id, yank);
        return yank;
    }

    AbilityBuild b;
    if (auto per = cat.effect_persistent(eff)) {
        b.def.kind = sim::EffectKind::CreatePersistent;
        b.def.amount = Fixed::from_double(per->amount);
        b.def.radius = Fixed::from_double(per->radius);
        b.def.period = Fixed::from_double(per->period);
        b.def.period_count = per->count;
        if (b.def.amount.raw() > 0) {
            sim::EffectNode dmg;
            dmg.kind = sim::EffectKind::Damage;
            dmg.amount = b.def.amount;
            sim::EffectNode pers;
            pers.kind = sim::EffectKind::CreatePersistent;
            pers.amount = b.def.amount;
            pers.radius = b.def.radius;
            pers.period = b.def.period;
            pers.period_count = b.def.period_count;
            pers.periodic = 0;
            b.tree = {dmg, pers};
            b.tree_root = 1;
        }
    } else if (auto dmg = cat.effect_damage(eff)) {
        b.def.kind = sim::EffectKind::Damage;
        b.def.amount = Fixed::from_int(*dmg);
    } else if (auto frac = cat.effect_energy_fraction(eff); frac && *frac > 0) {
        b.def.kind = sim::EffectKind::ModifyEnergy;
        b.def.energy_fraction = Fixed::from_double(*frac);
    } else if (auto heal = cat.effect_life_change(eff); heal && *heal > 0) {
        b.def.kind = sim::EffectKind::Heal;
        b.def.amount = Fixed::from_double(*heal);
    } else if (auto beh = cat.effect_apply_behavior(eff)) {
        auto bb = cat.behavior_buff(*beh);
        if (!bb) return std::nullopt;
        b.def.kind = sim::EffectKind::ApplyBehavior;
        b.def.duration = Fixed::from_double(bb->duration);
        b.def.move_speed_mult = Fixed::from_double(bb->move_speed_mult);
        b.def.attack_speed_mult = Fixed::from_double(bb->attack_speed_mult);
        b.def.beh_root = bb->root;
        b.def.beh_stun = bb->stun;
        b.def.beh_dmg_per_tick = Fixed::from_double(bb->dmg_per_tick);
        b.def.beh_dmg_period = Fixed::from_double(bb->dmg_period);
        if (bb->move_speed_mult == 1.0)
            if (auto slow = cat.effect_movement_slow_behavior(eff))
                if (auto sb = cat.behavior_buff(*slow); sb && sb->move_speed_mult < 1.0)
                    b.def.move_speed_mult = Fixed::from_double(sb->move_speed_mult);
    } else if (auto cu = cat.effect_create_unit(eff)) {
        b.def.kind = sim::EffectKind::CreateUnit;
        b.def.spawn_count = cu->count;
        b.spawn_unit = cu->unit;
        if (!cu->spawn_effect.empty())
            if (auto beh = cat.effect_apply_behavior(cu->spawn_effect))
                if (auto bb = cat.behavior_buff(*beh))
                    b.spawn_life = Fixed::from_double(bb->duration);
    } else {
        return std::nullopt;
    }
    if (auto r = cat.scalar("CAbil", abil_id, "Range"); r && !r->empty())
        b.def.range = Fixed::parse(*r);
    b.energy_cost = cabil_energy(cat, abil_id);
    b.life_cost = cabil_life(cat, abil_id);
    b.cooldown = cabil_cooldown(cat, abil_id);
    bind_cast_times(cat, abil_id, b);
    parse_ability_filters(cat, abil_id, b);
    parse_ability_validators(cat, eff, b);
    return b;
}

namespace {
bool local_is_point(const AbilityBuild& b, int32_t idx) {
    if (idx < 0 || idx >= int32_t(b.tree.size())) return false;
    const sim::EffectNode& n = b.tree[idx];
    if (n.kind == sim::EffectKind::EnumArea) return true;
    if (n.kind == sim::EffectKind::Set && !n.children.empty())
        return local_is_point(b, n.children[0]);
    return false;
}

uint32_t behavior_id_hash(const std::string& id) {
    uint32_t h = 2166136261u;
    for (unsigned char c : id) {
        h ^= c;
        h *= 16777619u;
    }
    return h;
}

int32_t build_effect_rec(const data::Catalog& cat, const std::string& eff, AbilityBuild& b,
                         int depth) {
    if (depth > 32 || eff.empty()) return -1;
    const std::string sub = cat.effect_subclass(eff);

    if (sub == "LaunchMissile") {
        const std::string impact = cat.scalar("CEffect", eff, "ImpactEffect").value_or("");
        return build_effect_rec(cat, impact, b, depth + 1);
    }
    if (sub == "EnumArea") {
        auto ea = cat.effect_enum_area(eff);
        if (!ea) return -1;
        const int32_t child = build_effect_rec(cat, ea->child, b, depth + 1);
        if (child < 0) return -1;
        sim::EffectNode n;
        n.kind = sim::EffectKind::EnumArea;
        n.radius = sim::Fixed::from_double(ea->radius);
        n.search_affiliation = cat.effect_search_affiliation(eff);
        n.children = {child};
        b.tree.push_back(std::move(n));
        return int32_t(b.tree.size()) - 1;
    }
    if (sub == "Set") {
        std::vector<int32_t> kids;
        for (const std::string& c : cat.effect_set_children(eff)) {
            const int32_t k = build_effect_rec(cat, c, b, depth + 1);
            if (k < 0) return -1;
            kids.push_back(k);
        }
        if (kids.empty()) return -1;
        const bool first_point = local_is_point(b, kids.front());
        for (int32_t k : kids)
            if (local_is_point(b, k) != first_point) return -1;
        sim::EffectNode n;
        n.kind = sim::EffectKind::Set;
        n.children = kids;
        if (!first_point) n.gates = parse_effect_vital_gates(cat, eff);
        b.tree.push_back(std::move(n));
        return int32_t(b.tree.size()) - 1;
    }
    if (sub == "Switch") {
        std::vector<int32_t> case_kids;
        std::vector<sim::VitalGate> case_gates;
        int32_t uncond = -1;
        for (const core::XmlNode& node : cat.resolved("CEffect", eff)) {
            if (node.name != "CaseArray") continue;
            const std::string* cef = node.attr("Effect");
            if (!cef || cef->empty()) continue;
            const std::string* vid = node.attr("Validator");
            if (!vid || vid->empty()) {
                uncond = build_effect_rec(cat, *cef, b, depth + 1);
                if (uncond < 0) return -1;
                break;
            }
            auto g = parse_vital_validator(cat, *vid);
            if (!g) continue;
            const int32_t k = build_effect_rec(cat, *cef, b, depth + 1);
            if (k < 0) continue;
            case_kids.push_back(k);
            case_gates.push_back(*g);
        }
        int32_t dk = uncond;
        if (dk < 0) {
            const std::string def = cat.scalar("CEffect", eff, "CaseDefault").value_or("");
            if (def.empty()) return -1;
            dk = build_effect_rec(cat, def, b, depth + 1);
            if (dk < 0) return -1;
        }
        sim::EffectNode n;
        n.kind = sim::EffectKind::Switch;
        n.children = case_kids;
        n.children.push_back(dk);
        n.switch_case_gates = case_gates;
        b.tree.push_back(std::move(n));
        return int32_t(b.tree.size()) - 1;
    }
    if (sub == "IssueOrder") {
        const std::string abil = cat.scalar("CEffect", eff, "Abil").value_or("");
        std::string low = abil;
        for (char& c : low)
            if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
        auto has = [&](const char* s) { return abil.find(s) != std::string::npos; };
        std::int16_t ord = -1;
        if (low == "move")
            ord = std::int16_t(sim::OrderType::Move);
        else if (low == "attack")
            ord = std::int16_t(sim::OrderType::Attack);
        else if (low == "patrol")
            ord = std::int16_t(sim::OrderType::Patrol);
        else if (low == "hold" || low == "holdposition")
            ord = std::int16_t(sim::OrderType::HoldPosition);
        else if (low == "stop")
            ord = sim::kIssueOrderStop;
        else if (low == "gather")
            ord = std::int16_t(sim::OrderType::Gather);
        else if (has("Stop"))
            ord = sim::kIssueOrderStop;
        else if (has("Gather"))
            ord = std::int16_t(sim::OrderType::Gather);
        else if (has("Hold"))
            ord = std::int16_t(sim::OrderType::HoldPosition);
        if (ord < 0) return -1;
        sim::EffectNode n;
        n.kind = sim::EffectKind::IssueOrder;
        n.issue_order = ord;
        const std::string wu = [&]() -> std::string {
            const auto& kids = cat.resolved("CEffect", eff);
            for (auto it = kids.rbegin(); it != kids.rend(); ++it) {
                if (it->name != "WhichUnit") continue;
                if (const std::string* v = it->attr("Value")) return *v;
                if (const std::string* v = it->attr("value")) return *v;
                return {};
            }
            return {};
        }();
        if (wu == "Caster" || wu == "CasterUnit" || wu == "Source") n.beh_on_caster = true;
        b.tree.push_back(std::move(n));
        return int32_t(b.tree.size()) - 1;
    }
    if (sub == "CreateHealer") {
        const auto rate = cat.scalar_num("CEffect", eff, "RechargeVitalRate");
        if (!rate || *rate <= 0) return -1;
        sim::EffectNode n;
        n.kind = sim::EffectKind::CreateHealer;
        n.amount = sim::Fixed::from_double(*rate);
        n.heal_drain_factor =
            sim::Fixed::from_double(cat.scalar_num("CEffect", eff, "DrainVitalCostFactor").value_or(0.0));
        b.tree.push_back(std::move(n));
        return int32_t(b.tree.size()) - 1;
    }
    if (sub == "Damage") {
        auto d = cat.effect_damage(eff);
        const double sb = cat.scalar_num("CEffect", eff, "ShieldBonus").value_or(0.0);
        if (!d && sb <= 0.0) return -1;
        sim::EffectNode n;
        n.kind = sim::EffectKind::Damage;
        n.amount = sim::Fixed::from_int(d.value_or(0));
        if (sb > 0.0) n.shield_bonus = sim::Fixed::from_double(sb);
        n.armor_reduction = sim::Fixed::from_double(cat.effect_armor_reduction(eff));
        n.gates = parse_effect_vital_gates(cat, eff);
        b.tree.push_back(std::move(n));
        return int32_t(b.tree.size()) - 1;
    }
    if (sub == "ApplyBehavior") {
        auto beh = cat.effect_apply_behavior(eff);
        if (!beh) return -1;
        auto bb = cat.behavior_buff(*beh);
        if (!bb) return -1;
        sim::EffectNode n;
        n.kind = sim::EffectKind::ApplyBehavior;
        n.duration = sim::Fixed::from_double(bb->duration);
        n.move_speed_mult = sim::Fixed::from_double(bb->move_speed_mult);
        n.attack_speed_mult = sim::Fixed::from_double(bb->attack_speed_mult);
        n.beh_root = bb->root;
        n.beh_stun = bb->stun;
        n.beh_control_transfer = bb->control_transfer;
        n.beh_dmg_per_tick = sim::Fixed::from_double(bb->dmg_per_tick);
        n.beh_dmg_period = sim::Fixed::from_double(bb->dmg_period);
        n.behavior_tag = behavior_id_hash(*beh);
        n.gates = parse_effect_vital_gates(cat, eff);
        b.tree.push_back(std::move(n));
        return int32_t(b.tree.size()) - 1;
    }
    if (sub == "RemoveBehavior") {
        auto bl = cat.scalar("CEffect", eff, "BehaviorLink");
        if (!bl || bl->empty()) return -1;
        sim::EffectNode n;
        n.kind = sim::EffectKind::RemoveBehavior;
        n.behavior_tag = behavior_id_hash(*bl);
        const std::string wu = [&]() -> std::string {
            const auto& kids = cat.resolved("CEffect", eff);
            for (auto it = kids.rbegin(); it != kids.rend(); ++it) {
                if (it->name != "WhichUnit") continue;
                if (const std::string* v = it->attr("Value")) return *v;
                if (const std::string* v = it->attr("value")) return *v;
                return {};
            }
            return {};
        }();
        if (wu == "Caster" || wu == "CasterUnit" || wu == "Source") n.beh_on_caster = true;
        b.tree.push_back(std::move(n));
        return int32_t(b.tree.size()) - 1;
    }
    if (sub == "ModifyUnit") {
        if (auto f = cat.effect_energy_fraction(eff); f && *f > 0) {
            sim::EffectNode n;
            n.kind = sim::EffectKind::ModifyEnergy;
            n.energy_fraction = sim::Fixed::from_double(*f);
            n.gates = parse_effect_vital_gates(cat, eff);
            b.tree.push_back(std::move(n));
            return int32_t(b.tree.size()) - 1;
        }
        if (auto h = cat.effect_life_change(eff); h && *h > 0) {
            sim::EffectNode n;
            n.kind = sim::EffectKind::Heal;
            n.amount = sim::Fixed::from_double(*h);
            n.gates = parse_effect_vital_gates(cat, eff);
            b.tree.push_back(std::move(n));
            return int32_t(b.tree.size()) - 1;
        }
        if (auto ec = cat.effect_flat_energy_change(eff); ec && *ec < 0.0) {
            sim::EffectNode n;
            n.kind = sim::EffectKind::DrainEnergy;
            n.amount = sim::Fixed::from_double(-*ec);
            n.gates = parse_effect_vital_gates(cat, eff);
            b.tree.push_back(std::move(n));
            return int32_t(b.tree.size()) - 1;
        }
        return -1;
    }
    return -1;
}
}

int32_t build_effect(const data::Catalog& cat, const std::string& effect_id, AbilityBuild& b) {
    const int32_t root = build_effect_rec(cat, effect_id, b, 0);
    b.tree_root = root;
    return root;
}

int32_t register_ability(sim::World& world, const AbilityBuild& b) {
    const int32_t n = int32_t(b.tree.size());
    if (b.tree.empty() || b.tree_root < 0 || b.tree_root >= n) return world.add_ability(b.def);

    std::vector<int32_t> remap(b.tree.size(), -1);
    auto to_global = [&](sim::EffectRef& e) {
        if (e >= 0 && e < n) e = remap[e];
    };
    for (int32_t i = 0; i < n; ++i) {
        sim::EffectNode node = b.tree[i];
        for (sim::EffectRef& c : node.children) to_global(c);
        to_global(node.initial);
        to_global(node.periodic);
        to_global(node.expire);
        to_global(node.impact);
        remap[i] = world.add_effect(std::move(node));
    }
    return world.add_ability_with_root(b.def, remap[b.tree_root]);
}

std::pair<int, int> morph_cost_delta(const data::Catalog& cat, const std::string& source_unit,
                                     const std::string& target_unit) {
    const data::UnitCost s = data::unit_cost(cat, source_unit);
    const data::UnitCost t = data::unit_cost(cat, target_unit);
    return {std::max(0, t.minerals - s.minerals), std::max(0, t.gas - s.gas)};
}

sim::Unit make_unit_upgraded(const data::Catalog& cat, const data::UpgradeCatalog& up,
                             const std::string& unit_id, int player,
                             const std::vector<std::string>& researched) {
    sim::Unit u = make_unit(cat, unit_id, player);
    apply_unit_upgrades(cat, up, unit_id, researched, u);
    return u;
}

bool order_repair(const data::Catalog& cat, sim::World& world, sim::RepairSystem& repairs,
                  std::uint32_t repairer, std::uint32_t target, const std::string& target_unit_id) {
    using sim::Fixed;
    const auto& us = world.units();
    if (repairer >= us.size() || target >= us.size() || repairer == target) return false;
    const sim::Unit& r = us[repairer];
    const sim::Unit& t = us[target];
    if (!r.alive || !t.alive) return false;
    if (!r.is_worker) return false;
    if (!world.are_allied(r.player, t.player)) return false;
    if (!(t.attributes & sim::kAttrMechanical)) return false;
    if (t.constructing) return false;
    if (t.hp >= t.hp_max) return false;
    const auto rt = cat.repair_time(target_unit_id);
    if (!rt || *rt <= 0.0) return false;
    const double hpmax = t.hp_max.to_double();
    if (hpmax <= 0.0) return false;
    const data::UnitCost cost = data::unit_cost(cat, target_unit_id);
    const Fixed rate = Fixed::from_double(hpmax / *rt);
    const Fixed min_per_hp = Fixed::from_double(double(cost.minerals) / hpmax);
    const Fixed gas_per_hp = Fixed::from_double(double(cost.gas) / hpmax);
    repairs.assign(repairer, target, rate, min_per_hp, gas_per_hp);
    return true;
}

int add_sensor_tower(const data::Catalog& cat, sim::SensorTowerSystem& towers, std::int32_t owner,
                     sim::Fixed x, sim::Fixed y, const std::string& unit_id) {
    const double r = cat.radar_range(unit_id);
    if (r <= 0.0) return -1;
    return towers.add(owner, x, y, sim::Fixed::from_double(r));
}

bool calldown_supply_drop(sim::World& world, sim::SupplyDropSystem& drops, std::uint32_t caster,
                          std::uint32_t depot) {
    using sim::Fixed;
    const auto& us = world.units();
    if (caster >= us.size() || depot >= us.size()) return false;
    if (!us[caster].alive || !us[depot].alive) return false;
    if (!world.are_allied(us[caster].player, us[depot].player)) return false;
    if (!(us[depot].attributes & sim::kAttrStructure)) return false;
    if (us[depot].supply_provides.raw() <= 0) return false;
    const Fixed cost = Fixed::from_int(50);
    if (us[caster].energy_max.raw() <= 0 || us[caster].energy < cost) return false;
    world.unit(caster).energy = world.unit(caster).energy - cost;
    drops.queue(depot, Fixed::from_double(sim::SupplyDropSystem::kEnRouteDelay));
    return true;
}

sim::EffectRef route_primary_weapon_through_executor(sim::World& world, std::uint32_t unit) {
    if (unit >= world.units().size()) return sim::kNoEffect;
    const sim::Unit& u = world.units()[unit];
    sim::EffectNode wn;
    wn.kind = sim::EffectKind::WeaponDamage;
    wn.amount = u.wpn_damage;
    wn.wpn_bonus_attr = u.wpn_bonus_attr;
    wn.wpn_bonus_amount = u.wpn_bonus_amount;
    const sim::EffectRef ref = world.add_effect(std::move(wn));
    world.unit(unit).wpn_effect = ref;
    return ref;
}

bool cast_abduct(sim::World& world, std::uint32_t caster, std::uint32_t target) {
    using sim::Fixed;
    const auto& us = world.units();
    if (caster >= us.size() || target >= us.size() || caster == target) return false;
    if (!us[caster].alive || !us[target].alive) return false;
    if (us[target].player < 0) return false;
    if (us[target].attributes & sim::kAttrStructure) return false;
    const Fixed cost = Fixed::from_int(75);
    if (us[caster].energy_max.raw() <= 0 || us[caster].energy < cost) return false;
    const Fixed dx = us[target].x - us[caster].x, dy = us[target].y - us[caster].y;
    const Fixed r = Fixed::from_int(9);
    if (dx * dx + dy * dy > r * r) return false;
    world.unit(caster).energy = world.unit(caster).energy - cost;
    world.unit(target).x = us[caster].x;
    world.unit(target).y = us[caster].y;
    return true;
}

}
