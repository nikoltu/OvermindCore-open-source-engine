#include "omc/app/ability.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <utility>

#include "omc/app/spawn.hpp"
#include "omc/core/xml.hpp"
#include "omc/data/economy.hpp"
#include "omc/sim/weapon.hpp"

namespace omc::app {

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

static void buff_attr_bonus(const data::Catalog::BehaviorBuff& bb, std::uint32_t& attr, Fixed& amount) {
    static const bool off = std::getenv("OMC_NO_BUFF_ATTR_BONUS") != nullptr;
    if (off) return;
    for (const auto& [tok, v] : bb.damage_attr_bonus) {
        const std::uint32_t bit = filter_attr_bit(tok);
        if (bit == 0 || v == 0.0) continue;
        attr = bit;
        amount = Fixed::from_double(v);
        return;
    }
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
        else if (t == "CanHaveEnergy") b.require_energy_pool = std::getenv("OMC_NO_CAN_HAVE_ENERGY") == nullptr;
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

namespace {
std::uint8_t target_filter_excluded_rel(const data::Catalog& cat, const std::string& abil_id, bool& authored);
}

std::uint8_t ability_target_excluded_rel(const data::Catalog& cat, const std::string& abil_id, bool& authored) {
    return target_filter_excluded_rel(cat, abil_id, authored);
}

AbilTargetMode ability_target_mode(const data::Catalog& cat, const std::string& abil_id) {
    if (const auto* e = cat.find("CAbil", abil_id); e && e->tag == "CAbilEffectInstant")
        return AbilTargetMode::Instant;
    auto b = make_ability(cat, abil_id);
    if (!b) return AbilTargetMode::Unit;
    const bool has_tree =
        !b->tree.empty() && b->tree_root >= 0 && b->tree_root < std::int32_t(b->tree.size());
    const sim::EffectKind kind = has_tree ? b->tree[b->tree_root].kind : b->def.kind;
    if (has_tree && kind == sim::EffectKind::LaunchMissile) {
        const std::uint8_t loc = b->tree[b->tree_root].missile_impact_loc;
        return (loc == 1 || loc == 3) ? AbilTargetMode::Point : AbilTargetMode::Unit;
    }
    if (has_tree && b->from_effect_tree && kind == sim::EffectKind::CreatePersistent) {
        const sim::EffectNode& rn = b->tree[b->tree_root];
        return (rn.persist_follow_target || rn.persist_follow_source) ? AbilTargetMode::Unit : AbilTargetMode::Point;
    }
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

namespace {
int32_t build_effect_rec(const data::Catalog& cat, const std::string& eff, AbilityBuild& b, int depth);

bool set_launches_missile(const data::Catalog& cat, const std::string& eff, int depth) {
    if (depth > 8 || eff.empty()) return false;
    const std::string sub = cat.effect_subclass(eff);
    if (sub == "LaunchMissile") return true;
    if (sub == "CreatePersistent") {
        for (const char* fld : {"InitialEffect", "ExpireEffect", "FinalEffect", "PeriodicEffect"})
            if (set_launches_missile(cat, cat.scalar("CEffect", eff, fld).value_or(""), depth + 1))
                return true;
        return false;
    }
    if (sub == "Set") {
        for (const std::string& c : cat.effect_set_children(eff))
            if (set_launches_missile(cat, c, depth + 1)) return true;
        return false;
    }
    if (sub == "Switch") {
        for (const core::XmlNode& node : cat.resolved("CEffect", eff))
            if (node.name == "CaseArray")
                if (const std::string* e = node.attr("Effect"); e && set_launches_missile(cat, *e, depth + 1)) return true;
        return set_launches_missile(cat, cat.scalar("CEffect", eff, "CaseDefault").value_or(""), depth + 1);
    }
    return false;
}

bool set_channels(const data::Catalog& cat, const std::string& eff, int depth) {
    if (depth > 8 || eff.empty()) return false;
    const std::string sub = cat.effect_subclass(eff);
    if (sub == "CreatePersistent")
        return cat.effect_flag_set(eff, "Channeled") && !cat.scalar("CEffect", eff, "ExpireEffect").value_or("").empty();
    if (sub == "Set") {
        for (const std::string& c : cat.effect_set_children(eff))
            if (set_channels(cat, c, depth + 1)) return true;
        return false;
    }
    if (sub == "Switch") {
        for (const core::XmlNode& node : cat.resolved("CEffect", eff))
            if (node.name == "CaseArray")
                if (const std::string* e = node.attr("Effect"); e && set_channels(cat, *e, depth + 1)) return true;
        return set_channels(cat, cat.scalar("CEffect", eff, "CaseDefault").value_or(""), depth + 1);
    }
    return false;
}
}

static void bind_cast_times(const data::Catalog& cat, const std::string& abil_id, AbilityBuild& b) {
    b.cast_intro_time = cabil_cast_phase(cat, abil_id, "CastIntroTime");
    b.cast_finish_time = cabil_cast_phase(cat, abil_id, "FinishTime");
    b.cast_outro_time = cabil_cast_phase(cat, abil_id, "CastOutroTime");
    b.def.prep_time = cabil_cast_phase(cat, abil_id, "PrepTime");
    b.def.cooldown = b.cooldown;
    b.def.abil_tag = behavior_id_hash(abil_id);
    if (auto rs = cat.scalar("CAbil", abil_id, "RangeSlop"); rs && !rs->empty()) b.def.range_slop = Fixed::parse(*rs);
    else b.def.range_slop = Fixed::from_int(1);
    b.def.uninterruptible = 0;
    b.def.cancel_effect_prep = sim::kNoEffect;
    for (const core::XmlNode& c : cat.resolved("CAbil", abil_id)) {
        if (c.name == "UninterruptibleArray") {
            const std::string ix = c.attr_or("index", "");
            const std::uint8_t bit = ix == "Prep" ? 1 : ix == "Cast" ? 2 : ix == "Channel" ? 4 : ix == "Finish" ? 8 : 0;
            if (c.attr_or("value", "1") != "0") b.def.uninterruptible |= bit;
            else b.def.uninterruptible &= std::uint8_t(~bit);
        } else if (c.name == "CancelEffect" && c.attr_or("index", "") == "Prep") {
            const std::string id = c.attr_or("value", "");
            if (!id.empty() && b.tree_root >= 0) b.def.cancel_effect_prep = build_effect_rec(cat, id, b, 1);
        }
    }
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
    u.armor_level = Fixed::from_double(fold_field(up, researched, "Unit", unit_name,
                                                  "LifeArmorLevel", u.armor_level.to_double()));

    u.speed        = Fixed::from_double(fold_field(up, researched, "Unit", unit_name, "Speed",
                                                   u.speed.to_double()));
    if (!std::getenv("OMC_UPGRADE_NO_ACCEL_FOLD"))
        u.accel = Fixed::from_double(fold_field(up, researched, "Unit", unit_name, "Acceleration",
                                                u.accel.to_double()));
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
    u.energy     = Fixed::from_double(fold_field(up, researched, "Unit", unit_name, "EnergyStart",
                                                 u.energy.to_double()));
    u.energy_regen = Fixed::from_double(fold_field(up, researched, "Unit", unit_name, "EnergyRegenRate",
                                                   u.energy_regen.to_double()));
    if (u.energy > u.energy_max) u.energy = u.energy_max;
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
    static const bool rate_last_wins = std::getenv("OMC_RATE_LAST_WINS") != nullptr;
    double rate[2] = {0.0, 0.0};
    if (rate_last_wins) {
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
    } else {
        for (std::size_t i = 0; i < 2 && i < weapons.size(); ++i) {
            const double seed = cat.scalar_num("CWeapon", weapons[i], "RateMultiplier").value_or(1.0);
            if (seed <= 0.0) continue;
            double v = seed;
            for (const std::string& upg : researched) {
                if (!up.contains(upg)) continue;
                const data::UpgradeEffect* last = nullptr;
                for (const data::UpgradeEffect& e : up.resolve(upg))
                    if (e.catalog == "Weapon" && e.field == "RateMultiplier" && e.entry == weapons[i]) last = &e;
                if (last) v = last->apply(v);
            }
            rate[i] = v / seed;
        }
    }
    for (double& r : rate)
        if (rate_last_wins) r = r != 0.0 ? 1.0 + r : 1.0;
    if (rate[0] != 1.0 && rate[0] > 0.0 && u.wpn_period.raw() > 0)
        u.wpn_period = u.wpn_period / Fixed::from_double(rate[0]);
    if (rate[1] != 1.0 && rate[1] > 0.0 && u.wpn2_period.raw() > 0)
        u.wpn2_period = u.wpn2_period / Fixed::from_double(rate[1]);
}

static UpgradeRevert snapshot_revert(std::uint32_t id, const sim::Unit& u) {
    return UpgradeRevert{id, u.wpn_damage, u.wpn2_damage, u.armor, u.speed, u.shield_armor,
                         u.hp_max, u.hp, u.energy_max, u.energy, u.wpn_range, u.wpn2_range,
                         u.wpn_period, u.wpn2_period, u.wpn_bonus_amount, u.shields_max, u.shields,
                         u.acquire_range, u.speed_mult_creep, u.burrow_speed, u.wpn_bonus_attr, u.accel};
}
static bool revert_differs(const UpgradeRevert& r, const sim::Unit& u) {
    return r.wpn_damage != u.wpn_damage || r.wpn2_damage != u.wpn2_damage || r.armor != u.armor ||
           r.speed != u.speed || r.shield_armor != u.shield_armor || r.hp_max != u.hp_max ||
           r.hp != u.hp || r.energy_max != u.energy_max || r.energy != u.energy ||
           r.wpn_range != u.wpn_range || r.wpn2_range != u.wpn2_range ||
           r.wpn_period != u.wpn_period || r.wpn2_period != u.wpn2_period ||
           r.wpn_bonus_amount != u.wpn_bonus_amount || r.wpn_bonus_attr != u.wpn_bonus_attr ||
           r.shields_max != u.shields_max || r.shields != u.shields || r.acquire_range != u.acquire_range ||
           r.speed_mult_creep != u.speed_mult_creep || r.burrow_speed != u.burrow_speed ||
           r.accel != u.accel;
}

std::size_t apply_upgrade_to_live(sim::World& w, const data::Catalog& cat,
                                  const data::UpgradeCatalog& up, const std::string& upgrade_id,
                                  int player, std::vector<UpgradeRevert>* revert, bool units) {
    if (!up.contains(upgrade_id)) return 0;
    const std::vector<std::string> just{upgrade_id};
    std::size_t pushed = 0;
    const std::vector<std::string>& list = w.researched(player);
    const auto at = std::find(list.begin(), list.end(), upgrade_id);
    const std::uint32_t idx = at == list.end() ? 0xFFFFFFFFu : std::uint32_t(at - list.begin());
    for (std::uint32_t id = 0; units && id < w.units().size(); ++id) {
        sim::Unit& u = w.unit(id);
        if (!u.alive || u.player != player || u.catalog_id.empty()) continue;
        const UpgradeRevert pre = snapshot_revert(id, u);
        apply_unit_upgrades(cat, up, u.catalog_id, just, u);
        if (u.upgrades_folded == idx) u.upgrades_folded = idx + 1;
        if (revert && revert_differs(pre, u)) {
            revert->push_back(pre);
            ++pushed;
        }
    }
    static const bool no_beh = std::getenv("OMC_UPGRADE_NO_BEHAVIOR_FOLD") != nullptr;
    if (!no_beh) {
        std::vector<std::pair<std::string, std::string>> done;
        for (const data::UpgradeEffect& e : up.resolve(upgrade_id)) {
            if (e.catalog != "Behavior" || e.entry.empty()) continue;
            const bool spd = e.field == "Modification.MoveSpeedMultiplier";
            const bool acc = e.field == "Modification.AccelerationMultiplier";
            if (!spd && !acc) continue;
            if (std::find(done.begin(), done.end(), std::make_pair(e.entry, e.field)) != done.end()) continue;
            done.emplace_back(e.entry, e.field);
            const auto bb = cat.behavior_buff(e.entry);
            if (!bb) continue;
            const std::uint32_t tag = behavior_id_hash(e.entry);
            sim::World::BehaviorUpgradeMod m;
            if (const sim::World::BehaviorUpgradeMod* cur = w.behavior_upgrade_mod(player, tag)) m = *cur;
            Fixed& slot = spd ? m.move_speed_mult : m.accel_mult;
            const double seed = slot.raw() >= 0 ? slot.to_double() : (spd ? bb->move_speed_mult : bb->accel_mult);
            slot = Fixed::from_double(fold_field(up, just, "Behavior", e.entry, e.field.c_str(), seed));
            w.put_behavior_upgrade_mod(player, tag, m);
        }
    }
    return pushed;
}

std::size_t fold_lagging_upgrades(sim::World& w, const data::Catalog& cat, const data::UpgradeCatalog& up) {
    std::size_t folded = 0;
    for (std::uint32_t id = 0; id < w.units().size(); ++id) {
        sim::Unit& u = w.unit(id);
        if (!u.alive || u.player < 0 || u.catalog_id.empty()) continue;
        const std::vector<std::string>& list = w.researched(u.player);
        if (u.upgrades_folded >= list.size()) continue;
        const std::vector<std::string> rest(list.begin() + std::ptrdiff_t(u.upgrades_folded), list.end());
        apply_unit_upgrades(cat, up, u.catalog_id, rest, u);
        u.upgrades_folded = std::uint32_t(list.size());
        ++folded;
    }
    return folded;
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
        u.accel = r.accel;
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

static std::optional<AbilityBuild> make_ability_cascade(const data::Catalog& cat, const std::string& abil_id,
                                                        bool& cascade) {
    cascade = false;
    if (const data::Catalog::Entry* re = cat.find("CAbil", abil_id);
        re && re->tag.rfind("CAbilRedirect", 0) == 0) {
        const std::string target = cat.scalar("CAbil", abil_id, "Abil").value_or("");
        if (target.empty()) return std::nullopt;
        AbilityBuild b;
        b.def.kind = sim::EffectKind::RedirectPassengers;
        std::string low = target;
        for (char& c : low)
            if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
        if (low == "stop") b.def.redirect_order = 0xFE;
        else if (low == "attack") b.def.redirect_order = std::uint8_t(sim::OrderType::Attack);
        else b.def.redirect_tag = behavior_id_hash(target);
        b.def.abil_tag = behavior_id_hash(abil_id);
        return b;
    }
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
            b.def.spawn_unit_tag = sim::type_identity(ob->unit);
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
        {
            std::vector<std::pair<std::string, double>> q;
            for (const core::XmlNode& f : cat.resolved("CAbil", abil_id))
                if (f.name == "Effect")
                    if (const std::string* v = f.attr("value"); v && !v->empty()) q.emplace_back(*v, 0.0);
            auto seen = [&](const std::string& id) {
                for (const auto& e : q)
                    if (e.first == id) return true;
                return false;
            };
            for (std::size_t qi = 0; qi < q.size() && qi < 96; ++qi) {
                const std::string id = q[qi].first;
                const double at = q[qi].second;
                double later = at;
                if (cat.effect_subclass(id) == "CreatePersistent") {
                    const double per = cat.array_links("CEffect", id, "PeriodicPeriodArray").empty()
                                           ? 0.0
                                           : std::strtod(cat.array_links("CEffect", id, "PeriodicPeriodArray")[0].c_str(), nullptr);
                    later += cat.scalar_num("CEffect", id, "InitialDelay").value_or(0.0) +
                             cat.scalar_num("CEffect", id, "PeriodCount").value_or(0.0) * per +
                             cat.scalar_num("CEffect", id, "ExpireDelay").value_or(0.0);
                }
                for (const core::XmlNode& f : cat.resolved("CEffect", id)) {
                    const bool after = f.name == "FinalEffect" || f.name == "ExpireEffect";
                    for (const char* key : {"value", "Effect"})
                        if (const std::string* v = f.attr(key); v && !v->empty() && cat.find("CEffect", *v) && !seen(*v))
                            q.emplace_back(*v, after ? later : at);
                }
            }
            std::vector<std::string> listened;
            for (const auto& e : q)
                for (const data::Catalog::ActorSubscription& s : cat.actors_on("Effect", e.first, "Start"))
                    for (const data::Catalog::ActorRule& r : cat.actor_rules(s.actor, s.family))
                        if (r.head == "Effect" && r.sub == "Start") listened.push_back(r.subject);
            for (const auto& e : q)
                if (!cat.actors_on("Effect", e.first, "Start").empty() ||
                    std::find(listened.begin(), listened.end(), e.first) != listened.end())
                    b.def.fx_spawn_tags.emplace_back(behavior_id_hash(e.first), Fixed::from_double(e.second));
        }
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

    if (cat.effect_subclass(eff) == "Set" && (set_launches_missile(cat, eff, 0) || set_channels(cat, eff, 0))) {
        AbilityBuild mine;
        if (build_effect(cat, eff, mine) >= 0) {
            mine.def.kind = mine.tree[mine.tree_root].kind;
            if (auto r = cat.scalar("CAbil", abil_id, "Range"); r && !r->empty())
                mine.def.range = Fixed::parse(*r);
            mine.energy_cost = cabil_energy(cat, abil_id);
            mine.life_cost = cabil_life(cat, abil_id);
            mine.cooldown = cabil_cooldown(cat, abil_id);
            bind_cast_times(cat, abil_id, mine);
            parse_ability_filters(cat, abil_id, mine);
            parse_ability_validators(cat, eff, mine);
            parse_ability_autocast_validators(cat, abil_id, mine);
            return mine;
        }
    }
    if (cat.effect_subclass(eff) == "LaunchMissile") {
        AbilityBuild missile;
        if (build_effect(cat, eff, missile) >= 0 &&
            (missile.tree[missile.tree_root].kind == sim::EffectKind::EnumArea ||
             missile.tree[missile.tree_root].kind == sim::EffectKind::LaunchMissile)) {
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
    if (cat.effect_subclass(eff) == "UseCalldown") {
        AbilityBuild cd;
        if (build_effect(cat, eff, cd) >= 0) {
            cd.def.kind = cd.tree[cd.tree_root].kind;
            if (auto r = cat.scalar("CAbil", abil_id, "Range"); r && !r->empty())
                cd.def.range = Fixed::parse(*r);
            cd.energy_cost = cabil_energy(cat, abil_id);
            cd.life_cost = cabil_life(cat, abil_id);
            cd.cooldown = cabil_cooldown(cat, abil_id);
            bind_cast_times(cat, abil_id, cd);
            parse_ability_filters(cat, abil_id, cd);
            parse_ability_validators(cat, eff, cd);
            return cd;
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

    if (cat.effect_subclass(eff) == "CreatePersistent") {
        bool precursor_jump = false;
        for (const std::string& pe : cat.array_links("CEffect", eff, "PeriodicEffectArray"))
            if (cat.effect_subclass(pe) == "CreateUnit")
                for (const core::XmlNode& f : cat.resolved("CEffect", pe))
                    if (f.name == "CreateFlags" && f.attr_or("index", "") == "Precursor" && f.attr_or("value", "1") != "0")
                        precursor_jump = true;
        if (precursor_jump) {
            AbilityBuild jump;
            if (build_effect(cat, eff, jump) >= 0) {
                jump.def.kind = jump.tree[jump.tree_root].kind;
                if (auto r = cat.scalar("CAbil", abil_id, "Range"); r && !r->empty()) jump.def.range = Fixed::parse(*r);
                jump.energy_cost = cabil_energy(cat, abil_id);
                jump.life_cost = cabil_life(cat, abil_id);
                jump.cooldown = cabil_cooldown(cat, abil_id);
                bind_cast_times(cat, abil_id, jump);
                parse_ability_filters(cat, abil_id, jump);
                return jump;
            }
        }
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
                field.fx_tag = behavior_id_hash(eff);
                apply.fx_beh_tag = behavior_id_hash(*beh);
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

    cascade = true;
    AbilityBuild b;
    if (auto per = cat.effect_persistent(eff)) {
        b.def.kind = sim::EffectKind::CreatePersistent;
        b.def.amount = Fixed::from_double(per->amount);
        b.def.radius = Fixed::from_double(per->radius);
        b.def.period = Fixed::from_double(per->period);
        b.def.period_count = per->count;
        b.def.persist_affiliation = per->affiliation;
        b.def.persist_excludes_structure = per->excludes_structure;
        std::string field = eff;
        {
            std::vector<std::string> q{eff};
            for (std::size_t qi = 0; qi < q.size() && qi < 64; ++qi) {
                if (cat.effect_subclass(q[qi]) == "CreatePersistent") {
                    field = q[qi];
                    break;
                }
                for (const core::XmlNode& f : cat.resolved("CEffect", q[qi]))
                    for (const char* key : {"value", "Effect"})
                        if (const std::string* v = f.attr(key); v && !v->empty() && cat.find("CEffect", *v) &&
                                                                std::find(q.begin(), q.end(), *v) == q.end())
                            q.push_back(*v);
            }
        }
        if (!cat.actors_on("Effect", field, "Start").empty() || !cat.actors_on("Effect", field, "Stop").empty())
            b.def.fx_tag = behavior_id_hash(field);
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
            static const bool kVictimBuff = std::getenv("OMC_PERSIST_NO_VICTIM_BUFF") == nullptr;
            const std::optional<std::string> vb = kVictimBuff ? cat.effect_apply_behavior(eff) : std::nullopt;
            const auto vbb = vb ? cat.behavior_buff(*vb) : std::nullopt;
            if (vb && vbb && vbb->duration > 0.0) {
                sim::EffectNode buff;
                buff.kind = sim::EffectKind::ApplyBehavior;
                buff.duration = Fixed::from_double(vbb->duration);
                buff.behavior_tag = behavior_id_hash(*vb);
                sim::EffectNode both;
                both.kind = sim::EffectKind::Set;
                both.children = {0, 1};
                sim::EffectNode field_node = b.tree[1];
                field_node.periodic = 2;
                b.tree = {b.tree[0], buff, both, field_node};
                b.tree_root = 3;
            }
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
        b.def.beh_suppress_cloak = bb->suppress_cloak;
        b.def.beh_disable_abils = bb->disable_abils;
        b.def.beh_disable_production = bb->disable_production;
        b.def.beh_disable_morph = bb->disable_morph;
        b.def.beh_disable_transport = bb->disable_transport;
        b.def.beh_weapon_range = sim::Fixed::from_double(bb->weapon_range_bonus);
        b.def.beh_decel_mult = sim::Fixed::from_double(bb->decel_mult);
        b.def.beh_accel_mult = sim::Fixed::from_double(bb->accel_mult);
        buff_attr_bonus(*bb, b.def.beh_wpn_bonus_attr, b.def.beh_wpn_bonus_amount);
        b.def.beh_suppress_collision = bb->suppress_collision;
        b.def.beh_suppress_passenger = bb->suppress_passenger;
        b.def.beh_speed_cap = bb->move_speed_max >= 0.0 ? sim::Fixed::from_double(bb->move_speed_max)
                                                        : sim::Fixed::from_int(-1);
        b.def.beh_disable_spawner = bb->disable_spawner;
        for (const std::string& a : bb->disabled_abils) b.def.beh_disabled_abils.push_back(behavior_id_hash(a));
        for (const std::string& a : bb->disabled_weapons) b.def.beh_disabled_weapons.push_back(behavior_id_hash(a));
        b.def.beh_benign = bb->benign;
        b.def.beh_passive = bb->passive;
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
        b.def.spawn_neutral = cu->spawn_owner == data::Catalog::SpawnOwner::Neutral;
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
    if (n.kind == sim::EffectKind::LaunchMissile)
        return n.missile_impact_loc == 1 || n.missile_impact_loc == 3;
    if (n.kind == sim::EffectKind::Set && !n.children.empty())
        return local_is_point(b, n.children[0]);
    return false;
}

std::string effect_cap_value(const data::Catalog& cat, const std::string& eff, const char* field) {
    const auto& kids = cat.resolved("CEffect", eff);
    for (auto it = kids.rbegin(); it != kids.rend(); ++it) {
        if (it->name != field) continue;
        if (const std::string* v = it->attr("Value")) return *v;
        if (const std::string* v = it->attr("value")) return *v;
        return {};
    }
    return {};
}
bool names_caster_location(const std::string& v) {
    return v == "CasterUnit" || v == "CasterPoint" || v == "SourceUnit" || v == "SourcePoint" ||
           v == "CasterUnitOrPoint" || v == "SourceUnitOrPoint";
}

void parse_search_tokens(const data::Catalog& cat, const std::string& eff, sim::EffectNode& n,
                         std::vector<std::string>& unknown) {
    const std::string sf = cat.scalar("CEffect", eff, "SearchFilters").value_or("");
    if (sf.empty()) return;
    const std::size_t semi = sf.find(';');
    const std::string halves[2] = {sf.substr(0, semi), semi == std::string::npos ? std::string() : sf.substr(semi + 1)};
    for (int h = 0; h < 2; ++h) {
        std::size_t s = 0;
        while (s < halves[h].size()) {
            std::size_t c = halves[h].find(',', s);
            if (c == std::string::npos) c = halves[h].size();
            const std::string t = halves[h].substr(s, c - s);
            s = c + 1;
            if (t.empty() || t == "-") continue;
            if (t == "Self" || t == "Player" || t == "Ally" || t == "Enemy" || t == "Neutral") continue;
            if (t == "Missile" || t == "Dead" || t == "Hidden" || t == "Visible" || t == "Stasis" || t == "Item" ||
                t == "Destructible" || t == "RawResource" || t == "HarvestableResource" || t == "Invulnerable")
                continue;
            if (t == "Ground") { (h == 0 ? n.search_req_plane : n.search_exc_plane) |= 1; continue; }
            if (t == "Air") { (h == 0 ? n.search_req_plane : n.search_exc_plane) |= 2; continue; }
            if (const std::uint32_t bit = filter_attr_bit(t)) {
                (h == 0 ? n.search_req_attr : n.search_exc_attr) |= bit;
                continue;
            }
            unknown.push_back(t);
        }
    }
}

void unread_modification(const data::Catalog& cat, const std::string& beh, std::vector<std::string>& out,
                         std::vector<std::string>& tolerated) {
    static const char* const kAttrs[] = {"MoveSpeedMultiplier", "DecelerationMultiplier", "MoveSpeedMaximum",
                                         "WeaponRange", "AttackSpeedMultiplier", "LifeArmorBonus", "ShieldArmorBonus",
                                         "Food"};
    static const char* const kState[] = {"Stun", "SuppressAttack", "Invulnerable", "Uncommandable", "Untargetable",
                                         "SuppressCloak", "SuppressCollision", "SuppressPassenger", "Benign", "Passive",
                                         "NoDraw", "Unselectable", "IsKiting", "Summoned"};
    static const char* const kAbilClass[] = {"CAbilAttack", "CAbilMove", "CAbilTrain", "CAbilQueue", "CAbilWarpTrain",
                                             "CAbilMorph", "CAbilMorphPlacement", "CAbilTransport"};
    auto in = [](const std::string& v, const char* const* list, std::size_t count) {
        for (std::size_t i = 0; i < count; ++i)
            if (v == list[i]) return true;
        return false;
    };
    for (const core::XmlNode& m : cat.resolved("CBehavior", beh)) {
        if (m.name != "Modification") continue;
        for (const auto& a : m.attrs)
            if (!in(a.first, kAttrs, sizeof(kAttrs) / sizeof(kAttrs[0]))) out.push_back("Modification." + a.first);
        for (const core::XmlNode& g : m.children) {
            const std::string idx = g.attr_or("index", ""), val = g.attr_or("value", "");
            if (in(g.name, kAttrs, sizeof(kAttrs) / sizeof(kAttrs[0])) || g.name == "RateMultiplierArray" ||
                g.name == "WeaponDisableArray" || g.name == "AbilLinkDisableArray")
                continue;
            if (g.name == "VitalMaxArray" && idx == "Life") continue;
            if (g.name == "StateFlags") {
                if (val == "0" || in(idx, kState, sizeof(kState) / sizeof(kState[0]))) continue;
            } else if (g.name == "ModifyFlags") {
                if (val == "0" || idx == "DisableAbils" || idx == "SuppressMoving" || idx == "SuppressTurning") continue;
            } else if (g.name == "AbilClassDisableArray") {
                if (val == "0" || in(idx, kAbilClass, sizeof(kAbilClass) / sizeof(kAbilClass[0]))) continue;
            } else if (g.name == "BehaviorLinkDisableArray") {
                if (val == "SpawnLarva") continue;
            }
            const std::string what = "Modification." + g.name + (idx.empty() ? std::string() : "[" + idx + "]");
            const bool soft = g.name == "AbilLinkEnableArray" || g.name == "AbilClassEnableArray" ||
                              g.name == "BehaviorLinkDisableArray" ||
                              (g.name == "StateFlags" && (idx == "SuppressDetection" || idx == "SuppressRadar"));
            (soft ? tolerated : out).push_back(what);
        }
    }
}

static int compare_code(const std::string& cmp) {
    if (cmp == "EQ") return 0;
    if (cmp == "NE") return 1;
    if (cmp == "LT") return 2;
    if (cmp == "LE") return 3;
    if (cmp == "GT") return 4;
    if (cmp == "GE") return 5;
    return -1;
}

bool g_full_validators = false;
std::vector<std::string>* g_full_tolerated = nullptr;

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
        if (beh.empty()) return -1;
        if (none && !strict && inverted == op.negate) {
            op.kind = sim::ValidatorOp::NoBehavior;
            op.behavior = behavior_id_hash(beh);
        } else {
            const int code = compare_code(cmp);
            if (code < 0) return -1;
            op.kind = sim::ValidatorOp::BehaviorCount;
            op.behavior = behavior_id_hash(beh);
            op.cmp = std::uint8_t(code);
            op.count_value = std::atoi(val.c_str());
        }
    } else if (tag == "CValidatorPlayerRequirement") {
        const std::string req = cat.scalar("CValidator", vid, "Value").value_or("");
        if (req.empty()) return -1;
        op.kind = sim::ValidatorOp::PlayerRequirement;
        op.requirement = req;
        op.find = cat.scalar("CValidator", vid, "Find").value_or("1") != "0";
        op.on_caster = true;
    } else if (tag == "CValidatorUnitOrderQueue") {
        const std::string link = cat.scalar("CValidator", vid, "AbilLink").value_or("");
        const std::string idx = cat.scalar("CValidator", vid, "AbilCmdIndex").value_or("");
        std::string low = link;
        for (char& c : low)
            if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
        std::uint16_t mask = 0;
        auto bit = [](sim::OrderType t) { return std::uint16_t(1u << int(t)); };
        if (low == "attack") mask = std::uint16_t(bit(sim::OrderType::Attack) | bit(sim::OrderType::AttackMove));
        else if (low == "move") mask = idx == "2" ? bit(sim::OrderType::HoldPosition) : bit(sim::OrderType::Move);
        else if (low == "patrol") mask = bit(sim::OrderType::Patrol);
        else if (low == "gather" || low == "harvest") mask = bit(sim::OrderType::Gather);
        else if (low == "stop") mask = 0;
        else return -1;
        op.kind = sim::ValidatorOp::OrderQueue;
        op.order_mask = mask;
        op.find = cat.scalar("CValidator", vid, "Find").value_or("1") != "0";
        if (cat.scalar("CValidator", vid, "WhichUnit").value_or("").empty() && which_unit(cat, "CValidator", vid).value.empty())
            op.on_caster = true;
    } else if (tag == "CValidatorLocationCompareRange") {
        auto cap = [&](const char* field) -> std::string {
            const auto& kids = cat.resolved("CValidator", vid);
            for (auto it = kids.rbegin(); it != kids.rend(); ++it) {
                if (it->name != field) continue;
                if (const std::string* v = it->attr("Value")) return *v;
                if (const std::string* v = it->attr("value")) return *v;
                return {};
            }
            return {};
        };
        const int code = compare_code(validator_field(cat, vid, tag, "Compare", "LT"));
        const std::string rs = cat.scalar("CValidator", vid, "Range").value_or("");
        if (code < 0 || rs.empty()) return -1;
        op.kind = sim::ValidatorOp::CompareRange;
        op.cmp = std::uint8_t(code);
        op.range = sim::Fixed::from_double(std::strtod(rs.c_str(), nullptr));
        op.loc_a_target = cap("WhichLocation").rfind("Target", 0) == 0;
        op.loc_b_target = cap("Value").rfind("Target", 0) == 0;
        op.on_caster = true;
    } else if (tag == "CValidatorUnitCompareCargo" || tag == "CValidatorUnitCompareResourceContents" ||
               tag == "CValidatorUnitCompareField" || tag == "CValidatorUnitCompareCooldown") {
        const int code = compare_code(validator_field(cat, vid, tag, "Compare", "EQ"));
        if (code < 0) return -1;
        op.kind = sim::ValidatorOp::Compare;
        op.cmp = std::uint8_t(code);
        op.value = sim::Fixed::from_double(std::strtod(validator_field(cat, vid, tag, "Value", "0").c_str(), nullptr));
        if (tag == "CValidatorUnitCompareCargo") {
            op.field = 0;
        } else if (tag == "CValidatorUnitCompareResourceContents") {
            op.field = 1;
        } else if (tag == "CValidatorUnitCompareField") {
            if (cat.scalar("CValidator", vid, "Field").value_or("") != "Radius") return -1;
            op.field = 2;
        } else {
            const std::string abil = cat.scalar("CValidator", vid, "Ability").value_or("");
            if (abil.empty()) return -1;
            op.field = 3;
            op.abil_tag = behavior_id_hash(abil);
        }
    } else if (tag == "CValidatorUnitMover") {
        const std::string mv = cat.scalar("CValidator", vid, "Value").value_or("");
        if (mv != "Fly" && mv != "Ground") return -1;
        op.kind = sim::ValidatorOp::Mover;
        op.want_fly = mv == "Fly";
        op.find = cat.scalar("CValidator", vid, "Find").value_or("1") != "0";
    } else if (tag == "CValidatorFunction") {
        if (vid != "Failure") return -1;
        op.kind = sim::ValidatorOp::ConstFalse;
    } else if (g_full_validators && tag == "CValidatorUnitCompareMarkerCount" && !cat.scalar("CValidator", vid, "Value")) {
        op.kind = sim::ValidatorOp::Filters;
        if (g_full_tolerated) g_full_tolerated->push_back("Validator " + vid + " (marker count, always passes)");
    } else if (tag == "CValidatorLocationEnumArea") {
        static const bool off = std::getenv("OMC_NO_ENUM_AREA_VALIDATOR") != nullptr;
        if (off) return -1;
        const core::XmlNode* area = nullptr;
        int areas = 0;
        for (const core::XmlNode& n : cat.resolved("CValidator", vid))
            if (n.name == "AreaArray") { area = &n; ++areas; }
        if (!area || areas != 1) return -1;
        std::string acmp = "GE", acount = "1", aradius, avalid;
        for (const core::XmlNode& c : area->children) {
            const std::string v = c.attr_or("value", "");
            if (c.name == "Compare") acmp = v;
            else if (c.name == "Count") acount = v;
            else if (c.name == "Radius") aradius = v;
            else if (c.name == "Validator") avalid = v;
        }
        if (const std::string* r = area->attr("Radius")) aradius = *r;
        if (const std::string* r = area->attr("Validator")) avalid = *r;
        const int acode = compare_code(acmp), ocode = compare_code(validator_field(cat, vid, tag, "Compare", "GE"));
        if (aradius.empty() || acode < 0 || ocode < 0) return -1;
        op.kind = sim::ValidatorOp::EnumArea;
        op.area_radius = sim::Fixed::from_double(std::strtod(aradius.c_str(), nullptr));
        op.area_cmp = std::uint8_t(acode);
        op.area_count = std::atoi(acount.c_str());
        op.cmp = std::uint8_t(ocode);
        op.count_value = std::atoi(validator_field(cat, vid, tag, "Count", "1").c_str());
        op.on_caster = false;
        std::string where;
        for (const core::XmlNode& n : cat.resolved("CValidator", vid))
            if (n.name == "WhichLocation") {
                if (const std::string* v = n.attr("Value")) where = *v;
                else if (const std::string* v2 = n.attr("value")) where = *v2;
            }
        op.loc_source = where == "SourceUnit" || where == "CasterUnit" || where == "SourcePoint" || where == "CasterPoint";
        for (const core::XmlNode& n : cat.resolved("CValidator", vid))
            if (n.name == "SearchFlags" && n.attr_or("index", "") == "ExtendByUnitRadius")
                op.area_extend = n.attr_or("value", "1") != "0";
        const std::string f = cat.scalar("CValidator", vid, "SearchFilters").value_or("");
        const std::size_t semi = f.find(';');
        const std::string req = f.substr(0, semi), exc = semi == std::string::npos ? "" : f.substr(semi + 1);
        bool ok = true;
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
            if (t == "-" || t == "Visible") return;
            if (t == "Ground") op.req_plane |= 1;
            else if (t == "Air") op.req_plane |= 2;
            else if (const std::uint32_t bit = filter_attr_bit(t)) op.req_attr |= bit;
            else ok = false;
        });
        each(exc, [&](const std::string& t) {
            if (t == "-" || t == "Missile" || t == "Dead" || t == "Hidden" || t == "Item" || t == "Stasis" ||
                t == "Destructible" || t == "RawResource" || t == "HarvestableResource" || t == "Visible")
                return;
            const std::uint8_t rel = t == "Self" ? 1 : t == "Player" ? 2 : t == "Ally" ? 4 : t == "Enemy" ? 8
                                   : t == "Neutral" ? 16 : 0;
            if (rel) op.exc_rel |= rel;
            else if (t == "Ground") op.exc_plane |= 1;
            else if (t == "Air") op.exc_plane |= 2;
            else if (t == "Worker") op.exc_worker = true;
            else if (t == "UnderConstruction") op.exc_constructing = true;
            else if (const std::uint32_t bit = filter_attr_bit(t)) op.exc_attr |= bit;
            else ok = false;
        });
        if (!ok) return -1;
        if (!avalid.empty()) {
            const int k = compile_validator(cat, avalid, ops, depth + 1, false, strict);
            if (k < 0) return -1;
            op.kids.push_back(std::uint16_t(k));
        }
    } else if (tag == "CValidatorLocationPathable") {
        op.kind = sim::ValidatorOp::Pathable;
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
        bool never = false;
        auto inert = [](const std::string& t) {
            return t == "Missile" || t == "Dead" || t == "Hidden" || t == "Item" || t == "Stasis" || t == "Destructible" ||
                   t == "RawResource" || t == "HarvestableResource" || t == "Visible";
        };
        each(req, [&](const std::string& t) {
            if (g_full_validators && t == "-") return;
            if (g_full_validators && t == "Visible") return;
            if (g_full_validators && (t == "Missile" || t == "Dead" || t == "Hidden" || t == "Item")) { never = true; return; }
            if (t == "Ground") { op.req_plane |= 1; modelled = true; }
            else if (t == "Air") { op.req_plane |= 2; modelled = true; }
            else if (const std::uint32_t bit = filter_attr_bit(t)) { op.req_attr |= bit; modelled = true; }
            else whole = false;
        });
        each(exc, [&](const std::string& t) {
            if (g_full_validators && (t == "-" || inert(t))) return;
            const std::uint8_t rel = t == "Self" ? 1 : t == "Player" ? 2 : t == "Ally" ? 4 : t == "Enemy" ? 8
                                   : t == "Neutral" ? 16 : 0;
            if (rel) { op.exc_rel |= rel; modelled = true; }
            else if (t == "Ground") { op.exc_plane |= 1; modelled = true; }
            else if (t == "Air") { op.exc_plane |= 2; modelled = true; }
            else if (t == "Invulnerable") { op.exc_invulnerable = true; modelled = true; }
            else if (const std::uint32_t bit = filter_attr_bit(t)) { op.exc_attr |= bit; modelled = true; }
            else whole = false;
        });
        if (g_full_validators && whole && never) {
            op.kind = sim::ValidatorOp::ConstFalse;
        } else if (g_full_validators && whole && !modelled) {
        } else if (!modelled || (!whole && (strict || inverted != op.negate))) return -1;
    } else {
        return -1;
    }
    ops.push_back(std::move(op));
    return int(ops.size()) - 1;
}

std::vector<std::string> effect_validator_ids(const data::Catalog& cat, const std::string& eff) {
    std::vector<std::string> slots;
    for (const core::XmlNode& f : cat.resolved("CEffect", eff)) {
        if (f.name != "ValidatorArray") continue;
        const std::string v = f.attr_or("value", "");
        if (const std::string* ix = f.attr("index"); ix && !ix->empty()) {
            const std::size_t k = std::size_t(std::strtoul(ix->c_str(), nullptr, 10));
            if (k >= 256) continue;
            if (slots.size() <= k) slots.resize(k + 1);
            slots[k] = v;
        } else {
            slots.push_back(v);
        }
    }
    return slots;
}

void attach_validators(const data::Catalog& cat, const std::string& eff, sim::EffectNode& n,
                       std::vector<std::string>* unbuilt = nullptr) {
    const std::vector<std::string> ids =
        unbuilt ? effect_validator_ids(cat, eff) : cat.array_links("CEffect", eff, "ValidatorArray");
    for (const std::string& vid : ids) {
        const data::Catalog::Entry* ve = cat.find("CValidator", vid);
        if (ve && ve->tag == "CValidatorUnitCompareVital" && !n.gates.empty()) continue;
        const int r = compile_validator(cat, vid, n.validators, 0, false, unbuilt != nullptr);
        if (r >= 0) n.validator_roots.push_back(std::uint16_t(r));
        else if (unbuilt && ve) unbuilt->push_back("Validator " + vid + " (" + ve->tag + ") on " + eff);
    }
}

int32_t build_effect_node(const data::Catalog& cat, const std::string& eff, AbilityBuild& b, int depth);

int32_t build_effect_rec(const data::Catalog& cat, const std::string& eff, AbilityBuild& b, int depth) {
    const int32_t idx = build_effect_node(cat, eff, b, depth);
    if (idx >= 0) attach_validators(cat, eff, b.tree[std::size_t(idx)], b.full_tree ? &b.unbuilt : nullptr);
    if (idx >= 0 && !eff.empty()) {
        sim::EffectNode& tn = b.tree[std::size_t(idx)];
        if (tn.fx_tag == 0 &&
            (!cat.actors_on("Effect", eff, "Start").empty() || !cat.actors_on("Effect", eff, "Stop").empty()))
            tn.fx_tag = behavior_id_hash(eff);
        static const bool kMissileOld = std::getenv("OMC_EFFECT_MISSILE_OLD") != nullptr;
        if (!kMissileOld && tn.kind == sim::EffectKind::LaunchMissile && tn.missile_tag == 0 && cat.weapon_missile_model_id(eff))
            tn.missile_tag = behavior_id_hash(eff);
    }
    else if (b.full_tree && !eff.empty()) b.unbuilt.push_back("Effect " + eff + " (" + cat.effect_subclass(eff) + ")");
    if (idx >= 0 && b.full_tree && cat.effect_subclass(eff) != "Teleport")
        for (const core::XmlNode& f : cat.resolved("CEffect", eff))
            if (f.name == "WhichUnit" || f.name == "ImpactUnit" || f.name == "LaunchUnit" || f.name == "WhichLocation" ||
                f.name == "ImpactLocation" || f.name == "LaunchLocation" || f.name == "TargetLocation")
                if (const std::string* fx = f.attr("Effect"); fx && !fx->empty())
                    b.unbuilt.push_back("Frame " + f.name + " Effect=" + *fx + " on " + eff);
    if (const char* t = std::getenv("OMC_ABIL_TRACE"); t && *t && *t != '0')
        std::fprintf(stderr, "[abil] %*s%s (%s) -> %s\n", depth * 2, "", eff.c_str(),
                     cat.effect_subclass(eff).c_str(), idx >= 0 ? "built" : "REFUSED");
    return idx;
}

int32_t build_effect_node(const data::Catalog& cat, const std::string& eff, AbilityBuild& b,
                          int depth) {
    if (depth > 32 || eff.empty()) return -1;
    const std::string sub = cat.effect_subclass(eff);

    if (sub == "CreateUnit") {
        bool precursor = false;
        for (const core::XmlNode& f : cat.resolved("CEffect", eff))
            if (f.name == "CreateFlags" && f.attr_or("index", "") == "Precursor") precursor = f.attr_or("value", "1") != "0";
        if (precursor) return build_effect_rec(cat, cat.scalar("CEffect", eff, "SpawnEffect").value_or(""), b, depth + 1);
        if (!b.full_tree) return -1;
        const std::string unit = cat.scalar("CEffect", eff, "SpawnUnit").value_or("");
        if (unit.empty() || !cat.find("CUnit", unit)) return -1;
        sim::EffectNode n;
        n.kind = sim::EffectKind::CreateUnit;
        n.spawn_count = std::max(1, int(cat.scalar_num("CEffect", eff, "SpawnCount").value_or(1)));
        n.spawn_neutral = effect_cap_value(cat, eff, "SpawnOwner") == "Neutral";
        n.area_at_source = names_caster_location(effect_cap_value(cat, eff, "WhichLocation"));
        n.spawn_exact = true;
        if (const std::string se = cat.scalar("CEffect", eff, "SpawnEffect").value_or(""); !se.empty()) {
            n.initial = build_effect_rec(cat, se, b, depth + 1);
            if (n.initial < 0) return -1;
        }
        for (const core::XmlNode& f : cat.resolved("CEffect", eff))
            if (f.name == "CreateFlags" && f.attr_or("value", "1") != "0") {
                const std::string ix = f.attr_or("index", "");
                if (ix == "SetFacing" || ix == "SetRallyPoint") b.unbuilt.push_back("CreateFlags " + ix + " on " + eff);
            }
        if (const std::string v = cat.scalar("CEffect", eff, "SpawnOffset").value_or(""); !v.empty() && v != "0,0")
            b.unbuilt.push_back("SpawnOffset on " + eff);
        b.tree.push_back(std::move(n));
        b.tree_spawns.emplace_back(int32_t(b.tree.size()) - 1, unit);
        return int32_t(b.tree.size()) - 1;
    }
    if (sub == "LaunchMissile") {
        const std::string impact = cat.scalar("CEffect", eff, "ImpactEffect").value_or("");
        int32_t child = -1;
        if (!impact.empty()) {
            child = build_effect_rec(cat, impact, b, depth + 1);
            if (child < 0) return -1;
        }
        if (b.full_tree) {
            if (const std::string fin = cat.scalar("CEffect", eff, "FinishEffect").value_or(""); !fin.empty()) {
                const int32_t fk = build_effect_rec(cat, fin, b, depth + 1);
                if (fk < 0) return -1;
                if (child < 0) {
                    child = fk;
                } else {
                    sim::EffectNode both;
                    both.kind = sim::EffectKind::Set;
                    both.children = {child, fk};
                    b.tree.push_back(std::move(both));
                    child = int32_t(b.tree.size()) - 1;
                }
            }
            for (const char* fld : {"LaunchEffect", "InterruptEffect"})
                if (const std::string v = cat.scalar("CEffect", eff, fld).value_or(""); !v.empty())
                    b.unbuilt.push_back(std::string(fld) + " " + v + " on " + eff);
        }
        const std::optional<double> speed = cat.effect_missile_speed(eff);
        if (!speed || *speed <= 0.0) return child;
        sim::EffectNode n;
        n.kind = sim::EffectKind::LaunchMissile;
        n.impact = child;
        n.missile_speed = sim::Fixed::from_double(*speed);
        auto loc = [&](const char* fld, std::uint8_t def) {
            std::uint8_t out = def;
            for (const core::XmlNode& x : cat.resolved("CEffect", eff)) {
                if (x.name != fld) continue;
                const std::string* v = x.attr("Value");
                if (!v) continue;
                if (*v == "SourceUnit") out = 0;
                else if (*v == "SourcePoint") out = 1;
                else if (*v == "TargetUnit") out = 2;
                else if (*v == "TargetPoint") out = 3;
            }
            return out;
        };
        n.missile_launch_loc = loc("LaunchLocation", 0);
        n.missile_impact_loc = loc("ImpactLocation", 2);
        auto off_z = [&](const char* fld) {
            const auto s = cat.scalar("CEffect", eff, fld);
            if (!s || s->empty()) return sim::Fixed();
            const std::size_t c1 = s->find(','), c2 = c1 == std::string::npos ? c1 : s->find(',', c1 + 1);
            if (c2 == std::string::npos) return sim::Fixed();
            return sim::Fixed::from_double(std::strtod(s->c_str() + c2 + 1, nullptr));
        };
        n.missile_never_instant = b.full_tree;
        n.missile_launch_z = off_z("LaunchOffset");
        n.missile_impact_z = off_z("ImpactOffset");
        b.tree.push_back(std::move(n));
        return int32_t(b.tree.size()) - 1;
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
        n.search_exclude_target = ea->exclude_target;
        n.search_affiliation = cat.effect_search_affiliation(eff);
        n.search_exclude_structure = cat.effect_search_excludes_structure(eff);
        if (b.full_tree) {
            std::vector<std::string> unknown;
            parse_search_tokens(cat, eff, n, unknown);
            for (const std::string& t : unknown) b.unbuilt.push_back("SearchFilters " + t + " on " + eff);
            n.area_at_source = names_caster_location(effect_cap_value(cat, eff, "ImpactLocation"));
            int rows = 0;
            for (const core::XmlNode& a : cat.resolved("CEffect", eff)) {
                if (a.name != "AreaArray") continue;
                if (!a.attr("index")) ++rows;
                for (const char* fld : {"RadiusBonus", "Arc", "Validator"})
                    if (const std::string* v = a.attr(fld); v && !v->empty() && *v != "0" && *v != "360")
                        b.unbuilt.push_back(std::string("AreaArray.") + fld + " on " + eff);
            }
            if (rows > 1) b.unbuilt.push_back("AreaArray rows > 1 on " + eff);
        }
        n.children = {child};
        b.tree.push_back(std::move(n));
        return int32_t(b.tree.size()) - 1;
    }
    if (sub == "CreatePersistent") {
        auto per = cat.effect_persistent(eff);
        sim::EffectNode n;
        n.kind = sim::EffectKind::CreatePersistent;
        if (per) {
            n.radius = sim::Fixed::from_double(per->radius);
            n.period = sim::Fixed::from_double(per->period);
            n.period_count = per->count;
            n.amount = sim::Fixed::from_double(per->amount);
            n.persist_affiliation = per->affiliation;
            n.persist_excludes_structure = per->excludes_structure;
        } else {
            const auto pp = cat.array_links("CEffect", eff, "PeriodicPeriodArray");
            if (!pp.empty() && !pp[0].empty()) n.period = sim::Fixed::from_double(std::strtod(pp[0].c_str(), nullptr));
            else if (auto p0 = cat.scalar_num("CEffect", eff, "Period")) n.period = sim::Fixed::from_double(*p0);
            n.period_count = int32_t(cat.scalar_num("CEffect", eff, "PeriodCount").value_or(0));
        }
        if (auto idl = cat.scalar_num("CEffect", eff, "InitialDelay"); idl && *idl > 0)
            n.persist_initial_delay = sim::Fixed::from_double(*idl);
        if (auto ed = cat.scalar_num("CEffect", eff, "ExpireDelay"); ed && *ed > 0)
            n.persist_expire_delay = sim::Fixed::from_double(*ed);
        if (const std::string pv = cat.scalar("CEffect", eff, "PeriodicValidator").value_or(""); !pv.empty()) {
            const int r = compile_validator(cat, pv, n.validators, 0, false);
            if (r >= 0) n.periodic_validator_roots.push_back(std::uint16_t(r));
        }
        static const bool countless_old = std::getenv("OMC_PERSIST_COUNTLESS_PERIODIC") != nullptr;
        const bool countless_timer = b.full_tree && !countless_old && !per && n.period_count <= 0 &&
                                     n.persist_expire_delay.raw() > 0 &&
                                     !cat.scalar("CEffect", eff, "ExpireEffect").value_or("").empty();
        for (const std::string& pe : cat.array_links("CEffect", eff, "PeriodicEffectArray")) {
            if (pe.empty() || countless_timer) continue;
            std::string leaf = pe;
            if (auto ea = cat.effect_enum_area(pe); ea && !(b.full_tree && !per)) leaf = ea->child;
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
        if (!per && n.initial < 0 && n.expire < 0 && n.final_effect < 0 && !(b.full_tree && n.periodic >= 0))
            return -1;
        n.persist_channeled = cat.effect_flag_set(eff, "Channeled");
        if (b.full_tree) {
            const std::string where = effect_cap_value(cat, eff, "WhichLocation");
            if (where == "TargetUnit") n.persist_follow_target = true;
            else if (where == "CasterUnit" || where == "SourceUnit") n.persist_follow_source = true;
            else if (!where.empty() && where != "TargetPoint" && where != "TargetUnitOrPoint")
                b.unbuilt.push_back("WhichLocation " + where + " on " + eff);
            std::string first;
            for (const std::string& pe : cat.array_links("CEffect", eff, "PeriodicEffectArray")) {
                if (pe.empty()) continue;
                if (first.empty()) first = pe;
                else if (pe != first) { b.unbuilt.push_back("PeriodicEffectArray varies on " + eff); break; }
            }
            if (n.periodic >= 0 && n.period_count <= 0) b.unbuilt.push_back("PeriodCount absent on " + eff);
            if (!cat.array_links("CEffect", eff, "PeriodicOffsetArray").empty())
                b.unbuilt.push_back("PeriodicOffsetArray on " + eff);
        }
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
        if (kids.empty() && !b.full_tree) return -1;
        auto neutral = [&](int32_t k) { return b.tree[std::size_t(k)].kind == sim::EffectKind::CreatePersistent; };
        int32_t first = -1;
        for (int32_t k : kids)
            if (!neutral(k)) { first = k; break; }
        const bool first_point = first >= 0 ? local_is_point(b, first) : true;
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
            if (r < 0 && b.full_tree) b.unbuilt.push_back("Switch case validator " + *vid + " on " + eff);
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
        for (const core::XmlNode& tl : cat.resolved("CEffect", eff))
            if (tl.name == "TargetLocation" && tl.attr("Effect") && !tl.attr("Effect")->empty() &&
                cap_value("TargetLocation") == "TargetPoint")
                n.teleport_to_cast_point = true;
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
        bool kill = false;
        for (const core::XmlNode& f : cat.resolved("CEffect", eff))
            if (f.name == "Flags" && f.attr_or("index", "") == "Kill" && f.attr_or("value", "1") != "0") kill = true;
        double frac_cur[3] = {0, 0, 0};
        bool unread_payload = false;
        if (b.full_tree)
            for (const core::XmlNode& f : cat.resolved("CEffect", eff)) {
                const double v = std::strtod(f.attr_or("value", "0").c_str(), nullptr);
                if (f.name == "VitalFractionCurrent") {
                    const std::string ix = f.attr_or("index", "");
                    const int k = ix == "Life" ? 0 : ix == "Shields" ? 1 : ix == "Energy" ? 2 : -1;
                    if (k >= 0) frac_cur[k] = v;
                } else if ((f.name == "VitalFractionMax" || f.name == "VitalBonus" || f.name == "LeechFraction") && v != 0) {
                    unread_payload = true;
                    b.unbuilt.push_back(f.name + " on " + eff);
                }
            }
        const bool has_frac = frac_cur[0] != 0 || frac_cur[1] != 0 || frac_cur[2] != 0;
        if (!d && sb <= 0.0 && !kill && !has_frac && !unread_payload) {
            if (cat.effect_flag_set(eff, "Notification")) {
                sim::EffectNode nn;
                nn.kind = sim::EffectKind::Set;
                b.tree.push_back(std::move(nn));
                return int32_t(b.tree.size()) - 1;
            }
            return -1;
        }
        sim::EffectNode n;
        n.kind = sim::EffectKind::Damage;
        n.damage_kill = kill;
        n.death_code = sim::death_code_of(cat.scalar("CEffect", eff, "Death").value_or(""));
        for (const core::XmlNode& r : cat.resolved("CEffect", eff)) {
            if (r.name == "AreaArray" && n.area_ring_count < 3) {
                const std::string rad = r.attr_or("Radius", "");
                if (rad.empty()) continue;
                n.area_ring_r[n.area_ring_count] = sim::Fixed::from_double(std::strtod(rad.c_str(), nullptr));
                const std::string fr = r.attr_or("Fraction", "1");
                n.area_ring_frac[n.area_ring_count] = sim::Fixed::from_double(std::strtod(fr.c_str(), nullptr));
                ++n.area_ring_count;
            } else if (r.name == "AttributeBonus") {
                const std::uint32_t bit = filter_attr_bit(r.attr_or("index", ""));
                const std::string v = r.attr_or("value", "");
                if (bit && !v.empty()) {
                    n.wpn_bonus_attr = bit;
                    n.wpn_bonus_amount = sim::Fixed::from_double(std::strtod(v.c_str(), nullptr));
                }
            }
        }
        if (n.area_ring_count > 0) {
            std::uint8_t mask = 31;
            const std::string sf = cat.scalar("CEffect", eff, "SearchFilters").value_or("");
            const std::size_t semi = sf.find(';');
            const std::string exc = semi == std::string::npos ? std::string() : sf.substr(semi + 1);
            auto has = [&](const char* tok) {
                const std::string t = std::string(",") + exc + ",";
                return t.find(std::string(",") + tok + ",") != std::string::npos;
            };
            if (has("Self")) mask &= std::uint8_t(~1u);
            if (has("Player")) mask &= std::uint8_t(~2u);
            if (has("Ally")) mask &= std::uint8_t(~4u);
            if (has("Enemy")) mask &= std::uint8_t(~8u);
            if (has("Neutral")) mask &= std::uint8_t(~16u);
            n.area_affiliation = mask;
        }
        n.amount = sim::Fixed::from_double(cat.effect_amount(eff).value_or(double(d.value_or(0))));
        if (sb > 0.0) n.shield_bonus = sim::Fixed::from_double(sb);
        if (b.full_tree) {
            n.dmg_frac_cur_life = sim::Fixed::from_double(frac_cur[0]);
            n.dmg_frac_cur_shields = sim::Fixed::from_double(frac_cur[1]);
            n.dmg_frac_cur_energy = sim::Fixed::from_double(frac_cur[2]);
            if (n.area_ring_count > 0) {
                n.area_at_source = names_caster_location(effect_cap_value(cat, eff, "ImpactLocation"));
                sim::EffectNode probe;
                std::vector<std::string> unknown;
                parse_search_tokens(cat, eff, probe, unknown);
                if (probe.search_req_attr || probe.search_exc_attr || probe.search_req_plane || probe.search_exc_plane ||
                    !unknown.empty())
                    b.unbuilt.push_back("Damage area SearchFilters on " + eff);
                for (const core::XmlNode& f : cat.resolved("CEffect", eff))
                    if (f.name == "ExcludeArray" && f.attr_or("Value", "") != "Outer")
                        b.unbuilt.push_back("Damage area ExcludeArray on " + eff);
            }
        }
        n.armor_reduction = sim::Fixed::from_double(cat.effect_armor_reduction(eff));
        n.damage_hidden = cat.effect_damage_hidden(eff);
        n.gates = parse_effect_vital_gates(cat, eff);
        b.tree.push_back(std::move(n));
        return int32_t(b.tree.size()) - 1;
    }
    if (sub == "ApplyBehavior") {
        auto beh = cat.effect_apply_behavior(eff);
        if (!beh) {
            bool names_behavior = false;
            for (const core::XmlNode& f : cat.resolved("CEffect", eff))
                if (f.name == "Behavior" && !f.attr_or("value", "").empty()) names_behavior = true;
            if (names_behavior) return -1;
            sim::EffectNode nn;
            nn.kind = sim::EffectKind::Set;
            b.tree.push_back(std::move(nn));
            return int32_t(b.tree.size()) - 1;
        }
        auto bb = cat.behavior_buff(*beh);
        if (!bb && cat.find("CBehavior", *beh)) {
            bb = data::Catalog::BehaviorBuff{};
            static const bool kNoLifecycle = std::getenv("OMC_TAG_BUFF_NO_LIFECYCLE") != nullptr;
            if (!kNoLifecycle) {
                bb->initial_effect = cat.scalar("CBehavior", *beh, "InitialEffect").value_or("");
                bb->expire_effect = cat.scalar("CBehavior", *beh, "ExpireEffect").value_or("");
                bb->final_effect = cat.scalar("CBehavior", *beh, "FinalEffect").value_or("");
            }
        }
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
        n.beh_suppress_cloak = bb->suppress_cloak;
        n.beh_disable_abils = bb->disable_abils;
        n.beh_disable_production = bb->disable_production;
        n.beh_disable_morph = bb->disable_morph;
        n.beh_disable_transport = bb->disable_transport;
        n.beh_weapon_range = sim::Fixed::from_double(bb->weapon_range_bonus);
        n.beh_decel_mult = sim::Fixed::from_double(bb->decel_mult);
        n.beh_accel_mult = sim::Fixed::from_double(bb->accel_mult);
        buff_attr_bonus(*bb, n.beh_wpn_bonus_attr, n.beh_wpn_bonus_amount);
        n.beh_suppress_collision = bb->suppress_collision;
        n.beh_suppress_passenger = bb->suppress_passenger;
        n.beh_speed_cap = bb->move_speed_max >= 0.0 ? sim::Fixed::from_double(bb->move_speed_max)
                                                    : sim::Fixed::from_int(-1);
        n.beh_disable_spawner = bb->disable_spawner;
        for (const std::string& a : bb->disabled_abils) n.beh_disabled_abils.push_back(behavior_id_hash(a));
        for (const std::string& a : bb->disabled_weapons) n.beh_disabled_weapons.push_back(behavior_id_hash(a));
        n.beh_benign = bb->benign;
        n.beh_passive = bb->passive;
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
        if (!bb->expire_effect.empty()) n.expire = build_effect_rec(cat, bb->expire_effect, b, depth + 1);
        if (const data::Catalog::DamageResponse dr = cat.behavior_damage_response(*beh); !dr.handled.empty())
            n.resp_handled = build_effect_rec(cat, dr.handled, b, depth + 1);
        n.beh_permanent = !bb->duration_authored && !cat.effect_apply_duration(eff).has_value();
        if (b.full_tree) {
            bool own_duration = false;
            for (const core::XmlNode& f : cat.resolved("CEffect", eff))
                if (f.name == "Flags" && f.attr_or("index", "") == "UseDuration") own_duration = f.attr_or("value", "1") != "0";
            if (own_duration && !cat.scalar_num("CEffect", eff, "Duration")) own_duration = false;
            n.beh_permanent = !bb->duration_authored && !own_duration;
            n.beh_food = sim::Fixed::from_double(bb->food);
            n.beh_life_max = sim::Fixed::from_double(bb->life_max_bonus);
            std::vector<std::string> unread, soft;
            unread_modification(cat, *beh, unread, soft);
            for (const std::string& u : unread) b.unbuilt.push_back("Behavior " + *beh + " " + u);
            for (const std::string& u : soft) b.tolerated.push_back("Behavior " + *beh + " " + u);
            const data::Catalog::DamageResponse dr = cat.behavior_damage_response(*beh);
            if (dr.present) {
                n.beh_resp_amount = sim::Fixed::from_double(dr.modify_amount);
                n.beh_resp_kinds = dr.kinds;
                if (dr.fraction != 1.0 || dr.modify_limit != 0.0 || dr.clamp_max != 0.0 || dr.clamp_min != 0.0 ||
                    dr.minimum != 0.0)
                    b.unbuilt.push_back("Behavior " + *beh + " DamageResponse fraction / limit / clamp");
            }
            if (!bb->periodic_effect.empty() && bb->dmg_per_tick <= 0.0 && bb->dmg_period <= 0.0)
                b.unbuilt.push_back("Behavior " + *beh + " PeriodicEffect without Period");
        }
        for (const std::string& vid : cat.array_links("CBehavior", *beh, "RemoveValidatorArray")) {
            const int r = compile_validator(cat, vid, n.beh_remove_ops, 0, false);
            if (r >= 0) n.beh_remove_roots.push_back(std::uint16_t(r));
        }
        if (which_unit(cat, "CEffect", eff).on_caster()) n.beh_on_caster = true;
        b.tree.push_back(std::move(n));
        return int32_t(b.tree.size()) - 1;
    }
    if (sub == "UseCalldown") {
        const std::string ce = cat.scalar("CEffect", eff, "CalldownEffect").value_or("");
        if (ce.empty()) return -1;
        std::string ammo;
        for (const std::string& ab : cat.ids("CAbil")) {
            const data::Catalog::Entry* ae = cat.find("CAbil", ab);
            if (!ae || ae->tag != "CAbilArmMagazine") continue;
            if (cat.scalar("CAbil", ab, "CalldownEffect").value_or("") != eff) continue;
            for (const core::XmlNode& info : cat.resolved("CAbil", ab))
                if (info.name == "InfoArray" && !info.attr_or("Unit", "").empty()) { ammo = info.attr_or("Unit", ""); break; }
            if (!ammo.empty()) break;
        }
        if (ammo.empty()) return -1;
        const int32_t child = build_effect_rec(cat, ce, b, depth + 1);
        if (child < 0) return -1;
        sim::EffectNode n;
        n.kind = sim::EffectKind::UseCalldown;
        n.children = {child};
        n.calldown_unit = ammo;
        n.calldown_count = std::max(1, int(cat.scalar_num("CEffect", eff, "CalldownCount").value_or(1)));
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
        if (b.full_tree) {
            bool salvage = false;
            double mf = 0, gf = 0;
            for (const core::XmlNode& f : cat.resolved("CEffect", eff)) {
                if (f.name == "ModifyFlags" && f.attr_or("index", "") == "Salvage") salvage = f.attr_or("value", "1") != "0";
                if (f.name != "SalvageFactor") continue;
                for (const core::XmlNode& r : f.children) {
                    if (r.name != "Resource") continue;
                    const double v = std::strtod(r.attr_or("value", "0").c_str(), nullptr);
                    if (r.attr_or("index", "") == "Minerals") mf = v < 0 ? -v : v;
                    else if (r.attr_or("index", "") == "Vespene") gf = v < 0 ? -v : v;
                }
            }
            if (salvage && (mf > 0 || gf > 0)) {
                sim::EffectNode n;
                n.kind = sim::EffectKind::ModifyVitals;
                n.salvage_mineral_frac = sim::Fixed::from_double(mf);
                n.salvage_gas_frac = sim::Fixed::from_double(gf);
                n.beh_on_caster = which_unit(cat, "CEffect", eff).on_caster();
                b.tree.push_back(std::move(n));
                b.tree_salvage.push_back(int32_t(b.tree.size()) - 1);
                return int32_t(b.tree.size()) - 1;
            }
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
        for (const core::XmlNode& f : cat.resolved("CEffect", eff)) {
            if (f.name != "Weapon" || f.attr_or("CooldownOperation", "") != "Set") continue;
            sim::EffectNode n;
            n.kind = sim::EffectKind::WeaponCooldown;
            n.wpn_cd_weapon = f.attr_or("Weapon", "");
            n.wpn_cd_fraction = sim::Fixed::from_double(std::strtod(f.attr_or("CooldownFraction", "1").c_str(), nullptr));
            std::string impact;
            for (const core::XmlNode& iu : cat.resolved("CEffect", eff))
                if (iu.name == "ImpactUnit")
                    if (const std::string* v = iu.attr("Value")) impact = *v;
            n.beh_on_caster = impact == "Source" || impact == "Caster";
            n.gates = parse_effect_vital_gates(cat, eff);
            b.tree.push_back(std::move(n));
            return int32_t(b.tree.size()) - 1;
        }
        {
            std::vector<int32_t> ops;
            std::string impact;
            for (const core::XmlNode& iu : cat.resolved("CEffect", eff))
                if (iu.name == "ImpactUnit")
                    if (const std::string* v = iu.attr("Value")) impact = *v;
            for (const core::XmlNode& f : cat.resolved("CEffect", eff)) {
                if (f.name != "Cost") continue;
                const std::string* abil = f.attr("Abil");
                if (!abil || abil->empty()) continue;
                sim::EffectNode n;
                n.kind = sim::EffectKind::ModifyVitals;
                n.cd_abil_tag = behavior_id_hash(abil->substr(0, abil->find(',')));
                n.cd_op = f.attr_or("CooldownOperation", "Add") == "Set" ? 1 : 0;
                n.cd_time = sim::Fixed::from_double(std::strtod(f.attr_or("CooldownTimeUse", "0").c_str(), nullptr));
                n.beh_on_caster = impact == "Source" || impact == "Caster";
                b.tree.push_back(std::move(n));
                ops.push_back(int32_t(b.tree.size()) - 1);
            }
            if (ops.size() == 1) return ops[0];
            if (ops.size() > 1) {
                sim::EffectNode set;
                set.kind = sim::EffectKind::Set;
                set.children = ops;
                b.tree.push_back(std::move(set));
                return int32_t(b.tree.size()) - 1;
            }
        }
        {
            bool has_vital_row = false, names_change = false;
            for (const core::XmlNode& f : cat.resolved("CEffect", eff)) {
                if (f.name != "VitalArray") continue;
                has_vital_row = true;
                if (f.attr("Change") || f.attr("ChangeFraction") || !f.children.empty())
                    names_change = true;
            }
            if (has_vital_row && !names_change) {
                sim::EffectNode nn;
                nn.kind = sim::EffectKind::Set;
                b.tree.push_back(std::move(nn));
                return int32_t(b.tree.size()) - 1;
            }
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
        to_global(node.resp_handled);
        remap[i] = world.add_effect(std::move(node));
    }
    sim::AbilityDef def = b.def;
    to_global(def.cancel_effect_prep);
    return world.add_ability_with_root(def, remap[b.tree_root]);
}

namespace {
bool env_flag(const char* name) {
    const char* v = std::getenv(name);
    return v && *v && *v != '0';
}

struct TreeShape {
    int payload = 0;
    bool edges = false;
    int32_t single = -1;
};
void tree_shape(const AbilityBuild& b, int32_t idx, TreeShape& s, int depth) {
    if (depth > 32 || idx < 0 || idx >= int32_t(b.tree.size())) return;
    const sim::EffectNode& n = b.tree[std::size_t(idx)];
    if (n.kind == sim::EffectKind::Set) {
        for (int32_t c : n.children) tree_shape(b, c, s, depth + 1);
        return;
    }
    ++s.payload;
    s.single = idx;
    if (n.initial >= 0 || n.periodic >= 0 || n.expire >= 0 || n.final_effect >= 0 || n.impact >= 0 ||
        n.resp_handled >= 0 || !n.children.empty())
        s.edges = true;
}

bool tree_adds(const AbilityBuild& t, const AbilityBuild& flat, bool instant) {
    if (flat.def.kind == sim::EffectKind::CreatePersistent && !flat.tree.empty()) return false;
    TreeShape s;
    tree_shape(t, t.tree_root, s, 0);
    if (s.payload == 0) return false;
    if (s.payload > 1 || s.edges) return true;
    const sim::EffectNode& n = t.tree[std::size_t(s.single)];
    if (n.kind != flat.def.kind) return true;
    if (n.kind == sim::EffectKind::ApplyBehavior) {
        if (n.beh_permanent || n.beh_apply_count > 1) return true;
        if (n.beh_on_caster && !instant) return true;
    }
    return false;
}

std::uint8_t target_filter_excluded_rel(const data::Catalog& cat, const std::string& abil_id, bool& authored) {
    std::string filters;
    for (const core::XmlNode& c : cat.resolved("CAbil", abil_id))
        if (c.name == "TargetFilters")
            if (const std::string* v = c.attr("value"); v && !v->empty()) filters = *v;
    authored = !filters.empty();
    const std::size_t semi = filters.find(';');
    if (semi == std::string::npos) return 0;
    const std::string exc = "," + filters.substr(semi + 1) + ",";
    auto has = [&](const char* tok) { return exc.find(std::string(",") + tok + ",") != std::string::npos; };
    std::uint8_t m = 0;
    if (has("Self")) m |= sim::kAffilSelf;
    if (has("Player")) m |= sim::kAffilPlayer;
    if (has("Ally")) m |= sim::kAffilAlly;
    if (has("Enemy")) m |= sim::kAffilEnemy;
    if (has("Neutral")) m |= sim::kAffilNeutral;
    return m;
}
}

std::optional<AbilityBuild> make_ability_flat(const data::Catalog& cat, const std::string& abil_id) {
    bool cascade = false;
    return make_ability_cascade(cat, abil_id, cascade);
}

std::optional<AbilityBuild> make_ability_ai(const data::Catalog& cat, const std::string& abil_id) {
    return env_flag("OMC_AI_EFFECT_TREE") ? make_ability(cat, abil_id) : make_ability_flat(cat, abil_id);
}

static bool build_generic_tree(const data::Catalog& cat, const std::string& abil_id, const std::string& eff,
                               const std::optional<AbilityBuild>& flat, AbilityBuild& t) {
    t.full_tree = true;
    g_full_validators = true;
    g_full_tolerated = &t.tolerated;
    const int32_t built = build_effect(cat, eff, t);
    g_full_validators = false;
    g_full_tolerated = nullptr;
    if (built < 0) return false;
    if (!t.unbuilt.empty()) return false;
    const data::Catalog::Entry* ae = cat.find("CAbil", abil_id);
    const bool instant = ae && ae->tag == "CAbilEffectInstant";
    if (flat && !tree_adds(t, *flat, instant)) return false;
    return true;
}

std::vector<std::string> effect_tree_gaps(const data::Catalog& cat, const std::string& abil_id) {
    bool cascade = false;
    const std::optional<AbilityBuild> flat = make_ability_cascade(cat, abil_id, cascade);
    const std::string eff = cat.cabil_effect_link(abil_id);
    if (!cascade || eff.empty()) return {};
    AbilityBuild t;
    build_generic_tree(cat, abil_id, eff, flat, t);
    return t.unbuilt;
}

std::optional<AbilityBuild> make_ability(const data::Catalog& cat, const std::string& abil_id) {
    bool cascade = false;
    std::optional<AbilityBuild> flat = make_ability_cascade(cat, abil_id, cascade);
    if (!cascade || env_flag("OMC_EFFECT_TREE_OFF")) return flat;
    const std::string eff = cat.cabil_effect_link(abil_id);
    if (eff.empty()) return flat;
    AbilityBuild t;
    if (!build_generic_tree(cat, abil_id, eff, flat, t)) return flat;
    if (flat) {
        t.def = flat->def;
        t.spawn_unit = flat->spawn_unit;
        t.spawn_life = flat->spawn_life;
    }
    t.def.kind = t.tree[std::size_t(t.tree_root)].kind;
    if (auto r = cat.scalar("CAbil", abil_id, "Range"); r && !r->empty()) t.def.range = Fixed::parse(*r);
    t.energy_cost = cabil_energy(cat, abil_id);
    t.life_cost = cabil_life(cat, abil_id);
    t.cooldown = cabil_cooldown(cat, abil_id);
    bind_cast_times(cat, abil_id, t);
    parse_ability_filters(cat, abil_id, t);
    parse_ability_validators(cat, eff, t);
    parse_ability_autocast_validators(cat, abil_id, t);
    t.target_exc_rel = target_filter_excluded_rel(cat, abil_id, t.target_rel_authored);
    t.from_effect_tree = true;
    return t;
}

std::pair<int, int> morph_cost_delta(const data::Catalog& cat, const std::string& source_unit,
                                     const std::string& target_unit) {
    const data::UnitCost s = data::unit_cost(cat, source_unit);
    const data::UnitCost t = data::unit_cost(cat, target_unit);
    return {std::max(0, t.minerals - s.minerals), std::max(0, t.gas - s.gas)};
}

std::pair<int, int> morph_charge(const data::Catalog& cat, const std::string& source_unit, const std::string& morph_abil) {
    const auto ms = cat.morph_spec(morph_abil);
    if (!ms || ms->target.empty()) return {0, 0};
    if (const auto mc = cat.morph_ability_cost(morph_abil); mc.found) return {mc.minerals, mc.gas};
    return morph_cost_delta(cat, source_unit, ms->target);
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
        const std::string v = n.attr_or("value", "1");
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
