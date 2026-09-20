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
    return data::fold_upgrade_field(up, researched, catalog, entry, field, base);
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
    const double base_shields_max = u.shields_max.to_double();
    const double new_shields_max = fold_field(up, researched, "Unit", unit_name, "ShieldsMax", base_shields_max);
    if (new_shields_max != base_shields_max) {
        u.shields_max = Fixed::from_double(new_shields_max);
        u.shields = u.shields + Fixed::from_double(new_shields_max - base_shields_max);
    }
    u.speed_mult_creep = Fixed::from_double(fold_field(up, researched, "Unit", unit_name, "SpeedMultiplierCreep",
                                                       u.speed_mult_creep.to_double()));
    if (!weapons.empty())
        u.wpn_range = Fixed::from_double(fold_field(up, researched, "Weapon", weapons[0], "Range",
                                                    u.wpn_range.to_double()));
    if (weapons.size() >= 2)
        u.wpn2_range = Fixed::from_double(fold_field(up, researched, "Weapon", weapons[1], "Range",
                                                     u.wpn2_range.to_double()));
    if (const std::string bv = cat.burrowed_variant(unit_name); !bv.empty()) {
        u.burrow_speed = Fixed::from_double(fold_field(up, researched, "Unit", bv, "Speed", u.burrow_speed.to_double()));
        u.can_burrow_move = u.burrow_speed.raw() > 0;
    }
    if (!weapons.empty())
        u.acquire_range = Fixed::from_double(fold_field(up, researched, "Weapon", weapons[0], "MinScanRange",
                                                        u.acquire_range.to_double()));
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
                         u.wpn_period, u.wpn2_period, u.wpn_bonus_amount, u.shields_max, u.shields,
                         u.acquire_range, u.speed_mult_creep, u.burrow_speed, u.wpn_bonus_attr};
}
static bool revert_differs(const UpgradeRevert& r, const sim::Unit& u) {
    return r.wpn_damage != u.wpn_damage || r.wpn2_damage != u.wpn2_damage || r.armor != u.armor ||
           r.speed != u.speed || r.shield_armor != u.shield_armor || r.hp_max != u.hp_max ||
           r.hp != u.hp || r.energy_max != u.energy_max || r.energy != u.energy ||
           r.wpn_range != u.wpn_range || r.wpn2_range != u.wpn2_range ||
           r.wpn_period != u.wpn_period || r.wpn2_period != u.wpn2_period ||
           r.wpn_bonus_amount != u.wpn_bonus_amount || r.wpn_bonus_attr != u.wpn_bonus_attr ||
           r.shields_max != u.shields_max || r.shields != u.shields || r.acquire_range != u.acquire_range ||
           r.speed_mult_creep != u.speed_mult_creep || r.burrow_speed != u.burrow_speed;
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
        u.wpn_bonus_amount = r.wpn_bonus_amount;
        u.wpn_bonus_attr = r.wpn_bonus_attr;
        u.shields_max = r.shields_max;
        u.shields = r.shields;
        u.acquire_range = r.acquire_range;
        u.speed_mult_creep = r.speed_mult_creep;
        u.burrow_speed = r.burrow_speed;
        u.can_burrow_move = r.burrow_speed.raw() > 0;
    }
}

namespace {
struct WhichUnitRef {
    std::string value, effect;
    bool on_caster() const { return value == "Caster" || value == "CasterUnit" || value == "Source"; }
};
}
static WhichUnitRef which_unit(const data::Catalog& cat, const char* family, const std::string& id) {
    WhichUnitRef w;
    const auto& kids = cat.resolved(family, id);
    for (auto it = kids.rbegin(); it != kids.rend(); ++it) {
        if (it->name != "WhichUnit") continue;
        if (const std::string* v = it->attr("Value")) w.value = *v;
        else if (const std::string* v2 = it->attr("value")) w.value = *v2;
        if (const std::string* e = it->attr("Effect")) w.effect = *e;
        break;
    }
    return w;
}

static std::string validator_field(const data::Catalog& cat, const std::string& vid, const std::string& tag,
                                   const char* field, const char* fallback) {
    if (auto v = cat.scalar("CValidator", vid, field)) return *v;
    if (auto d = cat.tag_default_scalar(tag, field); d && !d->empty()) return *d;
    return fallback;
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
    const std::string cmp = validator_field(cat, vid, ve->tag, "Compare", "LT");
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
static void parse_autocast_validators(const data::Catalog& cat, const std::string& vid, AbilityBuild& b,
                                      int depth = 0) {
    if (vid.empty() || depth > 4) return;
    const data::Catalog::Entry* ve = cat.find("CValidator", vid);
    if (!ve) return;
    if (ve->tag == "CValidatorCombine") {
        const bool all = validator_field(cat, vid, ve->tag, "Type", "Or") == "And";
        AbilityBuild one;
        int modelled = 0;
        for (const std::string& c : cat.array_links("CValidator", vid, "CombineArray")) {
            AbilityBuild& into = all ? b : one;
            const std::size_t g = into.autocast_caster_gates.size(), n = into.autocast_not_behaviors.size();
            parse_autocast_validators(cat, c, into, depth + 1);
            modelled += into.autocast_caster_gates.size() != g || into.autocast_not_behaviors.size() != n;
        }
        if (!all && modelled == 1) {
            b.autocast_caster_gates.insert(b.autocast_caster_gates.end(), one.autocast_caster_gates.begin(),
                                           one.autocast_caster_gates.end());
            b.autocast_not_behaviors.insert(b.autocast_not_behaviors.end(), one.autocast_not_behaviors.begin(),
                                            one.autocast_not_behaviors.end());
        }
        return;
    }
    const bool caster = which_unit(cat, "CValidator", vid).on_caster();
    if (ve->tag == "CValidatorUnitCompareVital" && caster) {
        if (auto g = parse_vital_validator(cat, vid)) b.autocast_caster_gates.push_back(*g);
        return;
    }
    if (ve->tag == "CValidatorUnitCompareBehaviorCount" && caster) {
        const std::string beh = cat.scalar("CValidator", vid, "Behavior").value_or("");
        const std::string cmp = validator_field(cat, vid, ve->tag, "Compare", "EQ");
        const std::string val = validator_field(cat, vid, ve->tag, "Value", "0");
        if (!beh.empty() && cmp == "EQ" && val == "0") b.autocast_not_behaviors.push_back(behavior_id_hash(beh));
    }
}

static void parse_ability_validators(const data::Catalog& cat, const std::string& eff,
                                     AbilityBuild& b) {
    auto gates = parse_effect_vital_gates(cat, eff);
    b.vital_gates.insert(b.vital_gates.end(), gates.begin(), gates.end());
}
void parse_ability_autocast_validators(const data::Catalog& cat, const std::string& abil_id, AbilityBuild& b) {
    for (const std::string& vid : cat.array_links("CAbil", abil_id, "AutoCastValidatorArray"))
        parse_autocast_validators(cat, vid, b);
}

namespace { struct cat_persist_tmp { double radius = 0, period = 0; int count = 0; }; }

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

    if (auto ob = cat.placed_obstacle_spec(abil_id)) {
        if (ob->ok()) {
            AbilityBuild b;
            b.def.kind = sim::EffectKind::CreateUnit;
            b.def.spawn_count = 1;
            b.spawn_unit = ob->unit;
            b.def.spawn_blocks_ground = true;
            b.def.radius = Fixed::from_double(ob->radius);
            b.def.duration = Fixed::from_double(ob->duration);
            if (auto r = cat.scalar("CAbil", abil_id, "Range"); r && !r->empty()) b.def.range = Fixed::parse(*r);
            b.energy_cost = Fixed::from_double(ob->energy);
            b.life_cost = cabil_life(cat, abil_id);
            parse_ability_filters(cat, abil_id, b);
            b.cooldown = cabil_cooldown(cat, abil_id);
            bind_cast_times(cat, abil_id, b);
            return b;
        }
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
        if (!ps.caster_behavior.empty()) {
            b.def.behavior_tag = behavior_id_hash(ps.caster_behavior);
            b.def.period = Fixed::from_double(ps.caster_duration);
        }
        if (!ps.cancel_behavior.empty()) b.def.phase_cancel_tag = behavior_id_hash(ps.cancel_behavior);
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
            parse_ability_autocast_validators(cat, abil_id, b);
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
    if (cat.effect_subclass(eff) == "Teleport") {
        AbilityBuild tp;
        if (build_effect(cat, eff, tp) >= 0) {
            tp.def.kind = tp.tree[tp.tree_root].kind;
            if (auto r = cat.scalar_num("CEffect", eff, "Range"); r && *r > 0)
                tp.def.range = Fixed::from_double(*r);
            else if (auto ar = cat.scalar("CAbil", abil_id, "Range"); ar && !ar->empty())
                tp.def.range = Fixed::parse(*ar);
            tp.energy_cost = cabil_energy(cat, abil_id);
            tp.life_cost = cabil_life(cat, abil_id);
            tp.cooldown = cabil_cooldown(cat, abil_id);
            bind_cast_times(cat, abil_id, tp);
            parse_ability_filters(cat, abil_id, tp);
            parse_ability_validators(cat, eff, tp);
            return tp;
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
            parse_ability_autocast_validators(cat, abil_id, heal);
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

    if (cat.effect_subclass(eff) == "CreatePersistent" && !cat.effect_damage(eff)) {
        if (auto beh = cat.effect_apply_behavior(eff)) {
            const auto dr = cat.behavior_damage_response(*beh);
            const auto bb = cat.behavior_buff(*beh);
            cat_persist_tmp per_v;
            per_v.count = int(cat.scalar_num("CEffect", eff, "PeriodCount").value_or(0));
            if (auto pp = cat.array_links("CEffect", eff, "PeriodicPeriodArray"); !pp.empty() && !pp[0].empty())
                per_v.period = std::strtod(pp[0].c_str(), nullptr);
            for (const std::string& pe : cat.array_links("CEffect", eff, "PeriodicEffectArray")) {
                for (const core::XmlNode& n : cat.resolved("CEffect", pe))
                    if (n.name == "AreaArray")
                        if (const std::string* r = n.attr("Radius"); r && !r->empty()) per_v.radius = std::strtod(r->c_str(), nullptr);
                if (per_v.radius > 0) break;
            }
            const cat_persist_tmp* per = &per_v;
            if (dr.present && dr.modify_amount < 0 && bb && per->radius > 0 && per->period > 0 && per->count > 0) {
                AbilityBuild g;
                sim::EffectNode apply;
                apply.kind = sim::EffectKind::ApplyBehavior;
                apply.duration = Fixed::from_double(bb->duration);
                apply.beh_resp_amount = Fixed::from_double(dr.modify_amount);
                apply.beh_resp_kinds = dr.kinds;
                sim::EffectNode field;
                field.kind = sim::EffectKind::CreatePersistent;
                field.radius = Fixed::from_double(per->radius);
                field.period = Fixed::from_double(per->period);
                field.period_count = per->count;
                field.periodic = 0;
                field.persist_allies = true;
                field.persist_follow_source = true;
                g.tree = {apply, field};
                g.tree_root = 1;
                g.def.kind = sim::EffectKind::CreatePersistent;
                g.def.radius = field.radius;
                g.def.period = field.period;
                g.def.period_count = field.period_count;
                g.energy_cost = cabil_energy(cat, abil_id);
                g.life_cost = cabil_life(cat, abil_id);
                g.cooldown = cabil_cooldown(cat, abil_id);
                bind_cast_times(cat, abil_id, g);
                return g;
            }
        }
    }

    AbilityBuild b;
    if (auto per = cat.effect_persistent(eff)) {
        b.def.kind = sim::EffectKind::CreatePersistent;
        b.def.amount = Fixed::from_double(per->amount);
        b.def.radius = Fixed::from_double(per->radius);
        b.def.period = Fixed::from_double(per->period);
        b.def.period_count = per->count;
        b.def.persist_affiliation = per->affiliation;
        b.def.persist_excludes_structure = per->excludes_structure;
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
            pers.persist_affiliation = b.def.persist_affiliation;
            pers.persist_excludes_structure = b.def.persist_excludes_structure;
            pers.persist_src = behavior_id_hash(eff);
            pers.periodic = 0;
            b.tree = {dmg, pers};
            b.tree_root = 1;
        }
    } else if (auto dmg = cat.effect_damage(eff)) {
        b.def.kind = sim::EffectKind::Damage;
        b.def.amount = cat.effect_subclass(eff) == "Damage"
                           ? Fixed::from_double(cat.effect_amount(eff).value_or(double(*dmg)))
                           : Fixed::from_int(*dmg);
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
        b.def.behavior_tag = behavior_id_hash(*beh);
        b.def.duration = Fixed::from_double(cat.effect_apply_duration(eff).value_or(bb->duration));
        b.def.move_speed_mult = Fixed::from_double(bb->move_speed_mult);
        b.def.attack_speed_mult = Fixed::from_double(bb->attack_speed_mult);
        b.def.beh_root = bb->root;
        b.def.beh_stun = bb->stun;
        b.def.beh_suppress_attack = bb->suppress_attack;
        b.def.beh_invulnerable = bb->invulnerable;
        b.def.beh_uncommandable = bb->uncommandable;
        b.def.beh_untargetable = bb->untargetable;
        b.def.beh_life_armor = sim::Fixed::from_double(bb->life_armor_bonus);
        b.def.beh_shield_armor = sim::Fixed::from_double(bb->shield_armor_bonus);
        b.def.beh_dmg_per_tick = Fixed::from_double(bb->dmg_per_tick);
        b.def.beh_rate_mult = Fixed::from_double(bb->rate_mult);
        b.def.beh_dmg_period = Fixed::from_double(bb->dmg_period);
        if (bb->move_speed_mult == 1.0)
            if (auto slow = cat.effect_movement_slow_behavior(eff))
                if (auto sb = cat.behavior_buff(*slow); sb && sb->move_speed_mult < 1.0)
                    b.def.move_speed_mult = Fixed::from_double(sb->move_speed_mult);
    } else if (auto cu = cat.effect_create_unit(eff)) {
        b.def.kind = sim::EffectKind::CreateUnit;
        b.def.spawn_count = cu->count;
        b.def.spawn_range = sim::Fixed::from_double(cu->spawn_range);
        b.spawn_unit = cu->unit;
        for (const std::string& beh : cat.effect_apply_behaviors(cu->spawn_effect)) {
            if (auto bb = cat.behavior_buff(beh); bb && bb->duration > 0 && b.spawn_life.raw() == 0)
                b.spawn_life = Fixed::from_double(bb->duration);
            if (const double f = cat.behavior_damage_response(beh).fraction; f > 0 && f != 1.0)
                b.spawn_dmg_fraction = Fixed::from_double(f);
            if (cat.behavior_no_damage_dealt(beh)) b.spawn_no_damage = true;
        }
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

int compile_validator(const data::Catalog& cat, const std::string& vid, std::vector<sim::ValidatorOp>& ops,
                      int depth, bool inverted, bool strict = false) {
    if (vid.empty() || depth > 8) return -1;
    const data::Catalog::Entry* ve = cat.find("CValidator", vid);
    if (!ve) return -1;
    sim::ValidatorOp op;
    op.negate = cat.scalar("CValidator", vid, "Negate").value_or("0") == "1";
    op.on_caster = which_unit(cat, "CValidator", vid).on_caster();
    const std::string& tag = ve->tag;
    if (tag == "CValidatorCombine") {
        op.kind = validator_field(cat, vid, tag, "Type", "Or") == "And" ? sim::ValidatorOp::And
                                                                         : sim::ValidatorOp::Or;
        const bool inv = inverted != op.negate;
        const bool may_drop = (op.kind == sim::ValidatorOp::And) != inv;
        for (const std::string& c : cat.array_links("CValidator", vid, "CombineArray")) {
            const int k = compile_validator(cat, c, ops, depth + 1, inv, strict);
            if (k < 0 && (strict || !may_drop)) return -1;
            if (k >= 0) op.kids.push_back(std::uint16_t(k));
        }
        if (op.kids.empty()) return -1;
    } else if (tag == "CValidatorUnitCompareVital") {
        auto g = parse_vital_validator(cat, vid);
        if (!g) return -1;
        op.kind = sim::ValidatorOp::Vital;
        op.vital = *g;
    } else if (tag == "CValidatorUnitType") {
        op.kind = sim::ValidatorOp::UnitType;
        op.unit_type = cat.scalar("CValidator", vid, "Value").value_or("");
        if (op.unit_type.empty()) return -1;
        if (cat.scalar("CValidator", vid, "Find").value_or("1") == "0") op.negate = !op.negate;
    } else if (tag == "CValidatorUnitCompareBehaviorCount") {
        const std::string beh = cat.scalar("CValidator", vid, "Behavior").value_or("");
        const std::string cmp = validator_field(cat, vid, tag, "Compare", "EQ");
        const std::string val = validator_field(cat, vid, tag, "Value", "0");
        const bool none = (cmp == "EQ" && val == "0") || (cmp == "LT" && val == "1") || (cmp == "LE" && val == "0");
        if (beh.empty() || !none || strict || (inverted != op.negate)) return -1;
        op.kind = sim::ValidatorOp::NoBehavior;
        op.behavior = behavior_id_hash(beh);
    } else if (tag == "CValidatorUnitFilters") {
        op.kind = sim::ValidatorOp::Filters;
        const std::string f = cat.scalar("CValidator", vid, "Filters").value_or("");
        const std::size_t semi = f.find(';');
        const std::string req = f.substr(0, semi), exc = semi == std::string::npos ? "" : f.substr(semi + 1);
        bool modelled = false, whole = true;
        auto each = [](const std::string& list, auto&& fn) {
            std::size_t s = 0;
            while (s < list.size()) {
                std::size_t c = list.find(',', s);
                if (c == std::string::npos) c = list.size();
                if (c > s) fn(list.substr(s, c - s));
                s = c + 1;
            }
        };
        each(req, [&](const std::string& t) {
            if (t == "Ground") { op.req_plane |= 1; modelled = true; }
            else if (t == "Air") { op.req_plane |= 2; modelled = true; }
            else if (const std::uint32_t bit = filter_attr_bit(t)) { op.req_attr |= bit; modelled = true; }
            else whole = false;
        });
        each(exc, [&](const std::string& t) {
            const std::uint8_t rel = t == "Self" ? 1 : t == "Player" ? 2 : t == "Ally" ? 4 : t == "Enemy" ? 8
                                   : t == "Neutral" ? 16 : 0;
            if (rel) { op.exc_rel |= rel; modelled = true; }
            else if (t == "Ground") { op.exc_plane |= 1; modelled = true; }
            else if (t == "Air") { op.exc_plane |= 2; modelled = true; }
            else if (t == "Invulnerable") { op.exc_invulnerable = true; modelled = true; }
            else if (const std::uint32_t bit = filter_attr_bit(t)) { op.exc_attr |= bit; modelled = true; }
            else whole = false;
        });
        if (!modelled || (!whole && (strict || inverted != op.negate))) return -1;
    } else {
        return -1;
    }
    ops.push_back(std::move(op));
    return int(ops.size()) - 1;
}

void attach_validators(const data::Catalog& cat, const std::string& eff, sim::EffectNode& n) {
    for (const std::string& vid : cat.array_links("CEffect", eff, "ValidatorArray")) {
        const data::Catalog::Entry* ve = cat.find("CValidator", vid);
        if (ve && ve->tag == "CValidatorUnitCompareVital" && !n.gates.empty()) continue;
        const int r = compile_validator(cat, vid, n.validators, 0, false);
        if (r >= 0) n.validator_roots.push_back(std::uint16_t(r));
    }
}

int32_t build_effect_node(const data::Catalog& cat, const std::string& eff, AbilityBuild& b, int depth);

int32_t build_effect_rec(const data::Catalog& cat, const std::string& eff, AbilityBuild& b, int depth) {
    const int32_t idx = build_effect_node(cat, eff, b, depth);
    if (idx >= 0) attach_validators(cat, eff, b.tree[std::size_t(idx)]);
    return idx;
}

int32_t build_effect_node(const data::Catalog& cat, const std::string& eff, AbilityBuild& b,
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
        n.search_max_count = ea->max_count;
        n.search_affiliation = cat.effect_search_affiliation(eff);
        n.children = {child};
        b.tree.push_back(std::move(n));
        return int32_t(b.tree.size()) - 1;
    }
    if (sub == "CreatePersistent") {
        auto per = cat.effect_persistent(eff);
        if (!per) return -1;
        sim::EffectNode n;
        n.kind = sim::EffectKind::CreatePersistent;
        n.radius = sim::Fixed::from_double(per->radius);
        n.period = sim::Fixed::from_double(per->period);
        n.period_count = per->count;
        n.amount = sim::Fixed::from_double(per->amount);
        n.persist_affiliation = per->affiliation;
        n.persist_excludes_structure = per->excludes_structure;
        for (const std::string& pe : cat.array_links("CEffect", eff, "PeriodicEffectArray")) {
            if (pe.empty()) continue;
            std::string leaf = pe;
            if (auto ea = cat.effect_enum_area(pe)) leaf = ea->child;
            const int32_t k = build_effect_rec(cat, leaf, b, depth + 1);
            if (k < 0) return -1;
            n.periodic = k;
            break;
        }
        auto edge = [&](const char* field, sim::EffectRef& slot) -> bool {
            const std::string id = cat.scalar("CEffect", eff, field).value_or("");
            if (id.empty()) return true;
            const int32_t k = build_effect_rec(cat, id, b, depth + 1);
            if (k < 0) return false;
            slot = k;
            return true;
        };
        if (!edge("InitialEffect", n.initial) || !edge("ExpireEffect", n.expire) || !edge("FinalEffect", n.final_effect))
            return -1;
        n.persist_src = behavior_id_hash(eff);
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
        std::vector<sim::ValidatorOp> case_ops;
        std::vector<std::uint16_t> case_roots;
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
            const int r = compile_validator(cat, *vid, case_ops, 0, false, true);
            if (r < 0) continue;
            const int32_t k = build_effect_rec(cat, *cef, b, depth + 1);
            if (k < 0) continue;
            case_kids.push_back(k);
            case_roots.push_back(std::uint16_t(r));
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
        n.switch_ops = std::move(case_ops);
        n.switch_case_roots = std::move(case_roots);
        b.tree.push_back(std::move(n));
        return int32_t(b.tree.size()) - 1;
    }
    if (sub == "Teleport") {
        auto cap_value = [&](const char* field) -> std::string {
            const auto& kids = cat.resolved("CEffect", eff);
            for (auto it = kids.rbegin(); it != kids.rend(); ++it) {
                if (it->name != field) continue;
                if (const std::string* v = it->attr("Value")) return *v;
                if (const std::string* v = it->attr("value")) return *v;
                return {};
            }
            return {};
        };
        const WhichUnitRef which = which_unit(cat, "CEffect", eff);
        if (!which.value.empty() && !which.on_caster()) return -1;
        sim::EffectNode n;
        n.kind = sim::EffectKind::Teleport;
        n.teleport_to_source = (cap_value("TargetLocation") == "SourcePoint");
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
        n.beh_on_caster = which_unit(cat, "CEffect", eff).on_caster();
        for (const core::XmlNode& f : cat.resolved("CEffect", eff)) {
            if (f.name != "CmdFlags") continue;
            if (f.attr_or("index", "") == "Queued") n.issue_queued = f.attr_or("value", "1") != "0";
        }
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
        if (cat.scalar("CEffect", eff, "RechargeVital").value_or("") == "Shields")
            n.heal_vital = sim::HealVital::Shields;
        if (const auto ie = cat.scalar("CEffect", eff, "InitialEffect"); ie && !ie->empty()) {
            const int32_t child = build_effect_rec(cat, *ie, b, depth + 1);
            if (child >= 0) n.children = {child};
        }
        b.tree.push_back(std::move(n));
        return int32_t(b.tree.size()) - 1;
    }
    if (sub == "Damage") {
        auto d = cat.effect_damage(eff);
        const double sb = cat.scalar_num("CEffect", eff, "ShieldBonus").value_or(0.0);
        if (!d && sb <= 0.0) return -1;
        sim::EffectNode n;
        n.kind = sim::EffectKind::Damage;
        n.amount = sim::Fixed::from_double(cat.effect_amount(eff).value_or(double(d.value_or(0))));
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
        n.duration = sim::Fixed::from_double(cat.effect_apply_duration(eff).value_or(bb->duration));
        n.move_speed_mult = sim::Fixed::from_double(bb->move_speed_mult);
        n.attack_speed_mult = sim::Fixed::from_double(bb->attack_speed_mult);
        n.beh_root = bb->root;
        n.beh_stun = bb->stun;
        n.beh_suppress_attack = bb->suppress_attack;
        n.beh_invulnerable = bb->invulnerable;
        n.beh_uncommandable = bb->uncommandable;
        n.beh_untargetable = bb->untargetable;
        n.beh_life_armor = sim::Fixed::from_double(bb->life_armor_bonus);
        n.beh_shield_armor = sim::Fixed::from_double(bb->shield_armor_bonus);
        n.beh_max_stack = bb->max_stack;
        n.beh_apply_count = std::max(1, int(cat.scalar_num("CEffect", eff, "Count").value_or(1)));
        n.beh_control_transfer = bb->control_transfer;
        n.beh_dmg_per_tick = sim::Fixed::from_double(bb->dmg_per_tick);
        n.beh_rate_mult = sim::Fixed::from_double(bb->rate_mult);
        n.beh_dmg_period = sim::Fixed::from_double(bb->dmg_period);
        n.behavior_tag = behavior_id_hash(*beh);
        n.gates = parse_effect_vital_gates(cat, eff);
        if (!bb->initial_effect.empty()) n.initial = build_effect_rec(cat, bb->initial_effect, b, depth + 1);
        if (!bb->periodic_effect.empty() && bb->dmg_per_tick <= 0.0 && bb->dmg_period > 0.0) {
            n.periodic = build_effect_rec(cat, bb->periodic_effect, b, depth + 1);
            n.period = sim::Fixed::from_double(bb->dmg_period);
        }
        if (!bb->final_effect.empty()) n.final_effect = build_effect_rec(cat, bb->final_effect, b, depth + 1);
        if (which_unit(cat, "CEffect", eff).on_caster()) n.beh_on_caster = true;
        b.tree.push_back(std::move(n));
        return int32_t(b.tree.size()) - 1;
    }
    if (sub == "DestroyPersistent") {
        auto cap = [&](const char* field) -> std::string {
            const auto& kids = cat.resolved("CEffect", eff);
            for (auto it = kids.rbegin(); it != kids.rend(); ++it) {
                if (it->name != field) continue;
                if (const std::string* v = it->attr("Value")) return *v;
                if (const std::string* v = it->attr("value")) return *v;
                return {};
            }
            return {};
        };
        sim::EffectNode n;
        n.kind = sim::EffectKind::DestroyPersistent;
        const std::string which = cat.scalar("CEffect", eff, "Effect").value_or("");
        n.destroy_src = which.empty() ? 0u : behavior_id_hash(which);
        n.destroy_count = int32_t(cat.scalar_num("CEffect", eff, "Count").value_or(0));
        n.radius = sim::Fixed::from_double(cat.scalar_num("CEffect", eff, "Radius").value_or(0));
        const std::string loc = cap("WhichLocation");
        n.destroy_at_source = loc == "SourcePoint" || loc == "CasterPoint" || loc == "SourceUnit" || loc == "CasterUnit";
        n.destroy_own_only = cap("OwningPlayer") == "Source";
        b.tree.push_back(std::move(n));
        return int32_t(b.tree.size()) - 1;
    }
    if (sub == "CancelOrder") {
        sim::EffectNode n;
        n.kind = sim::EffectKind::CancelOrder;
        const std::string cmd = cat.scalar("CEffect", eff, "AbilCmd").value_or("");
        n.cancel_kind = cmd.rfind("attack", 0) == 0 ? 1 : 0;
        n.cancel_active = true;
        for (const core::XmlNode& f : cat.resolved("CEffect", eff)) {
            if (f.name != "Flags") continue;
            const std::string idx = f.attr_or("index", "");
            const bool on = f.attr_or("value", "1") != "0";
            if (idx == "Active") n.cancel_active = on;
            else if (idx == "Queued") n.cancel_queued = on;
        }
        n.remove_count = int32_t(cat.scalar_num("CEffect", eff, "Count").value_or(0));
        n.beh_on_caster = which_unit(cat, "CEffect", eff).on_caster();
        b.tree.push_back(std::move(n));
        return int32_t(b.tree.size()) - 1;
    }
    if (sub == "TransferBehavior") {
        auto bl = cat.scalar("CEffect", eff, "Behavior");
        if (!bl || bl->empty()) return -1;
        auto ref = [&](const char* field) -> std::string {
            const auto& kids = cat.resolved("CEffect", eff);
            for (auto it = kids.rbegin(); it != kids.rend(); ++it) {
                if (it->name != field) continue;
                if (const std::string* v = it->attr("Value")) return *v;
                if (const std::string* v = it->attr("value")) return *v;
                return {};
            }
            return {};
        };
        auto is_caster = [](const std::string& v) { return v == "Caster" || v == "CasterUnit" || v == "Source" || v == "SourceUnit"; };
        sim::EffectNode n;
        n.kind = sim::EffectKind::TransferBehavior;
        n.behavior_tag = behavior_id_hash(*bl);
        n.transfer_from_caster = is_caster(ref("LaunchUnit"));
        n.transfer_to_caster = is_caster(ref("ImpactUnit"));
        n.transfer_copy = cat.scalar("CEffect", eff, "Copy").value_or("0") == "1";
        b.tree.push_back(std::move(n));
        return int32_t(b.tree.size()) - 1;
    }
    if (sub == "RemoveBehavior") {
        auto bl = cat.scalar("CEffect", eff, "BehaviorLink");
        if (!bl || bl->empty()) return -1;
        sim::EffectNode n;
        n.kind = sim::EffectKind::RemoveBehavior;
        n.behavior_tag = behavior_id_hash(*bl);
        n.beh_on_caster = which_unit(cat, "CEffect", eff).on_caster();
        n.remove_count = int32_t(cat.scalar_num("CEffect", eff, "Count").value_or(0));
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
        if (const auto vc = cat.effect_vital_change(eff); vc.any) {
            sim::EffectNode n;
            n.kind = sim::EffectKind::ModifyVitals;
            n.vit_life = sim::Fixed::from_double(vc.life);
            n.vit_life_frac = sim::Fixed::from_double(vc.life_frac);
            n.vit_shields = sim::Fixed::from_double(vc.shields);
            n.vit_shields_frac = sim::Fixed::from_double(vc.shields_frac);
            n.vit_energy = sim::Fixed::from_double(vc.energy);
            n.vit_energy_frac = sim::Fixed::from_double(vc.energy_frac);
            n.beh_on_caster = which_unit(cat, "CEffect", eff).on_caster();
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
        to_global(node.final_effect);
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

bool is_resource_drop_off(const data::Catalog& cat, const std::string& unit_id,
                          const std::string& resource) {
    if (unit_id.empty() || resource.empty()) return false;
    bool declared = false, set = false;
    for (const core::XmlNode& n : cat.resolved("CUnit", unit_id)) {
        if (n.name != "ResourceDropOff") continue;
        const std::string* idx = n.attr("index");
        if (!idx || *idx != resource) continue;
        declared = true;
        const std::string& v = n.attr_or("value", "1");
        set = !(v.empty() || v == "0" || v == "false");
    }
    return declared && set;
}

std::uint32_t nearest_drop_off(const data::Catalog& cat, const sim::World& world,
                               std::uint32_t worker) {
    const std::vector<sim::Unit>& us = world.units();
    if (worker >= us.size() || !us[worker].alive) return sim::kNoTarget;
    const sim::Unit& u = us[worker];
    const std::string resource = u.gathers_gas ? "Vespene" : "Minerals";
    std::uint32_t best = sim::kNoTarget;
    Fixed bd;
    for (const sim::Unit& o : us) {
        if (!o.alive || o.id == worker) continue;
        if (o.player != u.player) continue;
        if (o.constructing) continue;
        if (o.catalog_id.empty()) continue;
        if (!is_resource_drop_off(cat, o.catalog_id, resource)) continue;
        const Fixed dx = o.x - u.x, dy = o.y - u.y;
        const Fixed d = dx * dx + dy * dy;
        if (best == sim::kNoTarget || d < bd) {
            best = o.id;
            bd = d;
        }
    }
    return best;
}

bool order_return_cargo(const data::Catalog& cat, sim::World& world, std::uint32_t worker) {
    const std::vector<sim::Unit>& us = world.units();
    if (worker >= us.size()) return false;
    const sim::Unit& u = us[worker];
    if (!u.alive || !u.is_worker) return false;
    if (u.carrying <= 0) return false;
    const std::uint32_t hall = nearest_drop_off(cat, world, worker);
    if (hall == sim::kNoTarget) return false;
    world.unit(worker).home_depot = hall;
    world.issue(worker, sim::Order{sim::OrderType::Gather, Fixed(), Fixed(), sim::kNoTarget});
    return true;
}

}
