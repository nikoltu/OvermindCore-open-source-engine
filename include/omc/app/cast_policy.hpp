#pragma once

#include <string>
#include <unordered_map>
#include <vector>

#include "omc/app/ability.hpp"
#include "omc/app/spawn.hpp"
#include "omc/data/abil_charge.hpp"
#include "omc/data/catalog.hpp"
#include "omc/data/economy.hpp"
#include "omc/sim/ability_system.hpp"
#include "omc/sim/mule.hpp"
#include "omc/sim/world.hpp"

namespace omc::app {

class CastPolicy {
public:
    using Fixed = sim::Fixed;

    explicit CastPolicy(const data::Catalog& cat) : cat_(&cat) {}

    bool register_offensive(sim::World& world, const std::string& unit_id,
                            const std::string& abil_id) {
        auto b = make_ability_ai(*cat_, abil_id);
        if (!b) return false;
        if (const auto* e = cat_->find("CAbil", abil_id); e && e->tag == "CAbilEffectInstant")
            return false;
        const bool has_tree =
            !b->tree.empty() && b->tree_root >= 0 && b->tree_root < int32_t(b->tree.size());
        const sim::EffectNode* rn = has_tree ? &b->tree[b->tree_root] : nullptr;
        const sim::EffectKind eff_kind = rn ? rn->kind : b->def.kind;
        if (rn && rn->kind == sim::EffectKind::EnumArea && !(rn->search_affiliation & sim::kAffilEnemy))
            return false;
        if (eff_kind == sim::EffectKind::CreateUnit || eff_kind == sim::EffectKind::Morph) return false;
        if (eff_kind == sim::EffectKind::Reveal) return false;
        if (eff_kind == sim::EffectKind::Force) return false;
        if (eff_kind == sim::EffectKind::PhaseShift) return false;
        if (eff_kind == sim::EffectKind::Inject) return false;
        if (eff_kind == sim::EffectKind::RemoveBehavior) return false;
        if (eff_kind == sim::EffectKind::ApplyBehavior) {
            const bool root_ = rn ? rn->beh_root : b->def.beh_root;
            const bool stun_ = rn ? rn->beh_stun : b->def.beh_stun;
            const bool ctrl_ = rn ? rn->beh_control_transfer : b->def.beh_control_transfer;
            const sim::Fixed dot_ = rn ? rn->beh_dmg_per_tick : b->def.beh_dmg_per_tick;
            const sim::Fixed msm_ = rn ? rn->move_speed_mult : b->def.move_speed_mult;
            const sim::Fixed asm_ = rn ? rn->attack_speed_mult : b->def.attack_speed_mult;
            const auto kOne = sim::Fixed::from_int(1).raw();
            if (!ctrl_ && !root_ && !stun_ && dot_.raw() == 0 && msm_.raw() == kOne && asm_.raw() == kOne)
                return false;
        }
        Entry e = book_entry(world, unit_id, *b);
        e.point_cast = (eff_kind == sim::EffectKind::CreatePersistent ||
                        eff_kind == sim::EffectKind::EnumArea);
        if (e.point_cast) e.aoe_radius = rn ? rn->radius : b->def.radius;
        if (rn && rn->kind == sim::EffectKind::UseCalldown && std::getenv("OMC_AI_CAST_UNARMED_CALLDOWN") == nullptr) {
            e.calldown_unit = rn->calldown_unit;
            e.calldown_count = rn->calldown_count;
        }
        entries_.push_back(std::move(e));
        return true;
    }

    bool register_selfcast(sim::World& world, const std::string& unit_id, const std::string& abil_id,
                           Fixed engage_range) {
        auto b = make_ability_ai(*cat_, abil_id);
        if (!b) return false;
        const bool has_tree =
            !b->tree.empty() && b->tree_root >= 0 && b->tree_root < int32_t(b->tree.size());
        const sim::EffectKind eff_kind =
            has_tree ? b->tree[b->tree_root].kind : b->def.kind;
        if (eff_kind != sim::EffectKind::ApplyBehavior) return false;
        Entry e = book_entry(world, unit_id, *b);
        e.self_cast = true;
        e.engage_range = engage_range;
        entries_.push_back(std::move(e));
        return true;
    }

    bool register_support(sim::World& world, const std::string& unit_id, const std::string& abil_id,
                          Fixed range) {
        auto b = make_ability_ai(*cat_, abil_id);
        if (!b) return false;
        const bool has_tree =
            !b->tree.empty() && b->tree_root >= 0 && b->tree_root < int32_t(b->tree.size());
        const sim::EffectKind eff_kind = has_tree ? b->tree[b->tree_root].kind : b->def.kind;
        const bool own_only_tree = b->from_effect_tree && b->target_rel_authored &&
                                   (b->target_exc_rel & sim::kAffilEnemy) && !(b->target_exc_rel & sim::kAffilPlayer);
        if (eff_kind != sim::EffectKind::CreateHealer && eff_kind != sim::EffectKind::Heal && !own_only_tree) return false;
        Entry e = book_entry(world, unit_id, *b);
        e.support = true;
        e.engage_range = range;
        entries_.push_back(std::move(e));
        return true;
    }

    bool autocast_on(std::uint32_t unit, const std::string& abil, bool authored_default) const {
        auto it = autocast_toggle_.find(toggle_key(unit, abil));
        return it == autocast_toggle_.end() ? authored_default : it->second;
    }
    bool toggle_autocast(sim::World& world, std::uint32_t unit, const std::string& abil_id) {
        if (unit >= world.units().size()) return false;
        const sim::Unit& u = world.units()[unit];
        bool cur = autocast_on(unit, abil_id, cat_->ability_autocast(abil_id).on_by_default);
        for (const Entry& e : autocast_entries(world, u.catalog_id))
            if (e.abil_id == abil_id) { cur = autocast_on(unit, e.abil_id, e.autocast_default_on); break; }
        autocast_toggle_[toggle_key(unit, abil_id)] = !cur;
        return !cur;
    }
    bool autocast_state(sim::World& world, std::uint32_t unit, const std::string& abil_id) {
        if (unit >= world.units().size()) return false;
        const sim::Unit& u = world.units()[unit];
        for (const Entry& e : autocast_entries(world, u.catalog_id))
            if (e.abil_id == abil_id) return autocast_on(unit, e.abil_id, e.autocast_default_on);
        return autocast_on(unit, abil_id, cat_->ability_autocast(abil_id).on_by_default);
    }
    void set_autocast(std::uint32_t unit, const std::string& abil_id, bool on) {
        autocast_toggle_[toggle_key(unit, abil_id)] = on;
    }

    float cooldown_fraction(const sim::World& world, std::uint32_t caster_id,
                            const std::string& abil_id) const {
        if (caster_id >= world.units().size() || abil_id.empty()) return 0.0f;
        const std::string& unit_id = world.units()[caster_id].catalog_id;
        if (unit_id.empty()) return 0.0f;
        auto it = ondemand_.find(unit_id + '\x1f' + abil_id);
        if (it == ondemand_.end()) return 0.0f;
        const Entry& e = it->second;
        if (e.slot < 0 || e.cooldown.raw() <= 0) return 0.0f;
        const sim::Fixed left = world.ability_cooldown_left(caster_id, e.slot);
        if (left.raw() <= 0) return 0.0f;
        const float f = float(left.to_double() / e.cooldown.to_double());
        return f > 1.0f ? 1.0f : f;
    }

    int run_autocast(sim::World& world, int player) {
        int casts = 0;
        const auto& units = world.units();
        for (std::uint32_t ci = 0; ci < units.size(); ++ci) {
            const sim::Unit& u = units[ci];
            if (!u.alive || u.player != player || u.catalog_id.empty()) continue;
            for (const Entry& e : autocast_entries(world, u.catalog_id)) {
                if (!autocast_on(ci, e.abil_id, e.autocast_default_on)) continue;
                if (!abils_.ready(world, ci, e.slot)) continue;
                if (!autocast_caster_ok(u, e)) continue;
                const std::uint32_t tgt = e.support ? wounded_ally(world, u, e.engage_range, e.heal_vital)
                                                    : best_target(world, u, e);
                if (tgt == sim::kNoTarget) continue;
                static const bool kResumeOld = std::getenv("OMC_AUTOCAST_RESUME_OLD") != nullptr;
                sim::Order resume;
                bool has_resume = false;
                if (!kResumeOld && !u.orders.empty()) {
                    const sim::OrderType t = u.orders.front().type;
                    if (t == sim::OrderType::Move || t == sim::OrderType::AttackMove || t == sim::OrderType::Patrol) {
                        resume = u.orders.front();
                        has_resume = true;
                    }
                }
                if (abils_.try_cast(world, ci, e.slot, tgt)) {
                    if (has_resume) world.issue(ci, resume,  true,  false);
                    ++casts;
                    break;
                }
            }
        }
        return casts;
    }

    int run(sim::World& world, int player) {
        int casts = 0;
        const auto& units = world.units();
        for (std::uint32_t ci = 0; ci < units.size(); ++ci) {
            const sim::Unit& u = units[ci];
            if (!u.alive || u.player != player || u.catalog_id.empty()) continue;
            for (const Entry& e : entries_) {
                if (e.unit_id != u.catalog_id) continue;
                if (!abils_.ready(world, ci, e.slot)) continue;
                if (!e.calldown_unit.empty()) {
                    int stock = 0;
                    for (const sim::Unit& s : units)
                        if (s.alive && s.player == u.player && s.catalog_id == e.calldown_unit) ++stock;
                    if (stock < e.calldown_count) continue;
                }
                bool cast = false;
                if (e.self_cast) {
                    cast = enemy_within(world, u, e.engage_range) && abils_.try_cast(world, ci, e.slot, ci);
                } else if (e.support) {
                    const std::uint32_t tgt = wounded_ally(world, u, e.engage_range, e.heal_vital);
                    cast = tgt != sim::kNoTarget && abils_.try_cast(world, ci, e.slot, tgt);
                } else if (e.point_cast) {
                    Fixed px, py;
                    cast = best_point(world, u, e, px, py) &&
                           abils_.try_cast_point(world, ci, e.slot, px, py);
                } else {
                    const std::uint32_t tgt = best_target(world, u, e);
                    cast = tgt != sim::kNoTarget && abils_.try_cast(world, ci, e.slot, tgt);
                }
                if (cast) {
                    ++casts;
                    break;
                }
            }
        }
        return casts;
    }

    bool cast_now(sim::World& world, std::uint32_t caster_id, const std::string& abil_id,
                  std::uint32_t target_id) {
        const Entry* e = ensure_booked(world, caster_id, abil_id);
        if (!e) return false;
        if (caster_id >= world.units().size() || target_id >= world.units().size()) return false;
        const sim::Unit& c = world.units()[caster_id];
        const sim::Unit& t = world.units()[target_id];
        if (!c.alive || !t.alive) return false;
        if (e->rel_authored) {
            std::uint8_t rel = sim::kAffilEnemy;
            if (caster_id == target_id) rel = sim::kAffilSelf;
            else if (sim::World::is_neutral(t.player)) rel = sim::kAffilNeutral;
            else if (t.player == c.player) rel = sim::kAffilPlayer;
            else if (world.are_allied(c.player, t.player)) rel = sim::kAffilAlly;
            if (e->exc_rel & rel) return false;
        } else if (e->support) {
            if (t.player != c.player) return false;
        } else if (t.player == c.player && caster_id != target_id) {
            return false;
        }
        if (e->requires_ground && t.flying) return false;
        if (e->requires_spawner && t.spawner_proto < 0) return false;
        const Fixed dx = t.x - c.x, dy = t.y - c.y;
        const Fixed reach = e->range + c.radius + t.radius;
        static const bool kRangeOld = std::getenv("OMC_CAST_RANGE_REFUSE_OLD") != nullptr;
        const bool walks = !kRangeOld && (c.speed.raw() > 0 || (c.flying && c.speed_air.raw() > 0)) && !c.burrowed;
        if (!walks && dx * dx + dy * dy > reach * reach) return false;
        static const bool no_pool = [] { const char* v = std::getenv("OMC_NO_CHARGE_POOL"); return v && v[0] == '1'; }();
        const data::ChargeSpec ch = no_pool ? data::ChargeSpec{} : data::ability_cost_charge(*cat_, abil_id);
        const bool pooled = ch.found && ch.location == "Player" && ch.count_max > 0.0;
        const int32_t use = pooled ? std::max<int32_t>(1, int32_t(ch.count_use)) : 0;
        auto ticks = [](double s) { return int(s * double(sim::World::kTicksPerSecond) + 0.5); };
        const std::string link = ch.link.empty() ? abil_id : ch.link;
        const int32_t start = ch.count_start > 0.0 ? int32_t(ch.count_start) : int32_t(ch.count_max);
        if (pooled && world.charge_pool_count(c.player, link, start, int32_t(ch.count_max), ticks(ch.time_start),
                                              ticks(ch.time_use)) < use)
            return false;
        if (!abils_.try_cast(world, caster_id, e->slot, target_id)) return false;
        if (pooled)
            world.charge_pool_take(c.player, link, start, int32_t(ch.count_max), ticks(ch.time_start), ticks(ch.time_use), use);
        return true;
    }

    bool cast_now_point(sim::World& world, std::uint32_t caster_id, const std::string& abil_id, Fixed px,
                        Fixed py) {
        const Entry* e = ensure_booked(world, caster_id, abil_id);
        if (!e) return false;
        if (caster_id >= world.units().size()) return false;
        const sim::Unit& c = world.units()[caster_id];
        if (!c.alive) return false;
        bool walk_point = false;
        if (e->requires_creep) {
            if (!world.creep().is_creep(px, py)) return false;
        } else if (e->range.raw() > 0) {
            const Fixed dx = px - c.x, dy = py - c.y;
            const Fixed reach = e->range + c.radius;
            static const bool kRangeOldPt = std::getenv("OMC_CAST_RANGE_REFUSE_OLD") != nullptr;
            const bool walks_pt = !kRangeOldPt && (c.speed.raw() > 0 || (c.flying && c.speed_air.raw() > 0)) && !c.burrowed;
            if (dx * dx + dy * dy > reach * reach) {
                if (!walks_pt) return false;
                walk_point = true;
            }
        }
        static const bool no_pool = [] { const char* v = std::getenv("OMC_NO_CHARGE_POOL"); return v && v[0] == '1'; }();
        const data::ChargeSpec ch = no_pool ? data::ChargeSpec{} : data::ability_cost_charge(*cat_, abil_id);
        const bool pooled = ch.found && ch.location == "Player" && ch.count_max > 0.0;
        const int32_t use = pooled ? std::max<int32_t>(1, int32_t(ch.count_use)) : 0;
        auto ticks = [](double s) { return int(s * double(sim::World::kTicksPerSecond) + 0.5); };
        const std::string link = ch.link.empty() ? abil_id : ch.link;
        const int32_t start = ch.count_start > 0.0 ? int32_t(ch.count_start) : int32_t(ch.count_max);
        if (pooled && world.charge_pool_count(c.player, link, start, int32_t(ch.count_max), ticks(ch.time_start),
                                              ticks(ch.time_use)) < use)
            return false;
        if (!abils_.try_cast_point(world, caster_id, e->slot, px, py)) return false;
        if (walk_point && !world.units()[caster_id].orders.empty() &&
            world.units()[caster_id].orders.front().type == sim::OrderType::Cast)
            world.unit(caster_id).orders.front().walk_in = true;
        if (pooled)
            world.charge_pool_take(c.player, link, start, int32_t(ch.count_max), ticks(ch.time_start), ticks(ch.time_use), use);
        return true;
    }

    sim::AbilitySystem& system() { return abils_; }

private:
    struct Entry {
        std::string unit_id;
        int32_t slot = -1;
        Fixed range;
        std::uint32_t req_attr = 0;
        std::uint32_t exc_attr = 0;
        bool req_energy = false;
        bool point_cast = false;
        Fixed aoe_radius;
        bool self_cast = false;
        bool support = false;
        sim::HealVital heal_vital = sim::HealVital::Life;
        Fixed engage_range;
        sim::Fixed cooldown;
        std::string abil_id;
        bool autocast_default_on = false;
        std::vector<sim::VitalGate> autocast_caster_gates;
        std::vector<std::uint32_t> autocast_not_behaviors;
        bool requires_creep = false;
        bool requires_ground = false;
        bool rel_authored = false;
        std::uint8_t exc_rel = 0;
        std::string calldown_unit;
        int32_t calldown_count = 1;
        bool requires_spawner = false;
    };

    static bool autocast_caster_ok(const sim::Unit& u, const Entry& e) {
        for (const sim::VitalGate& g : e.autocast_caster_gates)
            if (!sim::vital_gate_passes(g, u)) return false;
        for (std::uint32_t tag : e.autocast_not_behaviors)
            for (const sim::BehaviorInstance& b : u.behaviors)
                if (b.src == tag) return false;
        return true;
    }

    static bool enemy_within(const sim::World& world, const sim::Unit& caster, Fixed range) {
        for (const sim::Unit& c : world.units()) {
            if (!c.alive || world.are_allied(c.player, caster.player) || c.player < 0) continue;
            const Fixed dx = c.x - caster.x, dy = c.y - caster.y;
            const Fixed reach = range + caster.radius + c.radius;
            if (dx * dx + dy * dy <= reach * reach) return true;
        }
        return false;
    }

    static std::uint32_t wounded_ally(const sim::World& world, const sim::Unit& caster, Fixed range,
                                      sim::HealVital vital = sim::HealVital::Life) {
        std::uint32_t best = sim::kNoTarget;
        Fixed best_d2;
        for (const sim::Unit& o : world.units()) {
            if (!o.alive || o.id == caster.id || o.player != caster.player) continue;
            if (vital == sim::HealVital::Shields ? (o.shields_max.raw() <= 0 || o.shields >= o.shields_max)
                                                 : o.hp >= o.hp_max)
                continue;
            if (o.loaded) continue;
            const Fixed dx = o.x - caster.x, dy = o.y - caster.y;
            const Fixed reach = range + caster.radius + o.radius;
            const Fixed d2 = dx * dx + dy * dy;
            if (d2 > reach * reach) continue;
            if (best == sim::kNoTarget || d2 < best_d2) {
                best = o.id;
                best_d2 = d2;
            }
        }
        return best;
    }

    const std::vector<Entry>& autocast_entries(sim::World& world, const std::string& unit_id) {
        auto it = autocast_cache_.find(unit_id);
        if (it != autocast_cache_.end()) return it->second;
        std::vector<Entry> out;
        for (const data::Catalog::AbilityCardButton& btn : cat_->unit_ability_card_buttons(unit_id)) {
            const auto ac = cat_->ability_autocast(btn.ability);
            if (!ac.offered || ac.range <= 0.0) continue;
            auto b = make_ability(*cat_, btn.ability);
            if (!b) continue;
            Entry e = book_entry(world, unit_id, *b);
            e.support = ac.targets_allies;
            e.engage_range = Fixed::from_double(ac.range);
            e.range = e.engage_range;
            e.abil_id = btn.ability;
            e.autocast_default_on = ac.on_by_default;
            e.autocast_caster_gates = b->autocast_caster_gates;
            e.autocast_not_behaviors = b->autocast_not_behaviors;
            out.push_back(std::move(e));
        }
        return autocast_cache_.emplace(unit_id, std::move(out)).first->second;
    }

    Entry book_entry(sim::World& world, const std::string& unit_id, const AbilityBuild& b) {
        sim::AbilitySpec spec;
        spec.effect = register_ability(world, b);
        spec.energy_cost = b.energy_cost;
        spec.life_cost = b.life_cost;
        spec.cooldown = b.cooldown;
        spec.req_attr_mask = b.req_attr_mask;
        spec.exc_attr_mask = b.exc_attr_mask;
        spec.require_energy = b.require_energy;
        spec.require_energy_pool = b.require_energy_pool;
        spec.vital_gates = b.vital_gates;
        Entry e;
        e.unit_id = unit_id;
        e.slot = abils_.add(spec);
        e.range = b.def.range;
        e.req_attr = b.req_attr_mask;
        e.exc_attr = b.exc_attr_mask;
        e.req_energy = b.require_energy;
        e.requires_ground = b.requires_ground;
        e.cooldown = b.cooldown;
        if (!b.tree.empty() && b.tree_root >= 0 && b.tree_root < int32_t(b.tree.size()))
            e.heal_vital = b.tree[b.tree_root].heal_vital;
        return e;
    }

    const Entry* ensure_booked(sim::World& world, std::uint32_t caster_id, const std::string& abil_id) {
        if (caster_id >= world.units().size()) return nullptr;
        const std::string& unit_id = world.units()[caster_id].catalog_id;
        if (unit_id.empty() || abil_id.empty()) return nullptr;
        const std::string key = unit_id + '\x1f' + abil_id;
        auto it = ondemand_.find(key);
        if (it != ondemand_.end()) return &it->second;
        auto b = make_ability(*cat_, abil_id);
        if (!b) return nullptr;
        if ((b->def.kind == sim::EffectKind::CreateUnit || b->def.kind == sim::EffectKind::PhaseShift) &&
            !b->spawn_unit.empty() && b->def.spawn_proto < 0) {
            sim::Unit proto = make_unit(*cat_, b->spawn_unit, 0);
            if (b->spawn_life.raw() > 0) proto.time_life = b->spawn_life;
            if (b->spawn_dmg_fraction.raw() != sim::Fixed::from_int(1).raw())
                proto.dmg_fraction = b->spawn_dmg_fraction;
            proto.deals_no_damage = b->spawn_no_damage;
            if (b->def.spawn_snap_resource) {
                const sim::MuleSystem::MuleStats st;
                const data::Catalog::HarvestSpec hs = cat_->harvest_spec(b->spawn_unit);
                proto.carry_amount = hs.amount_request > 0 ? hs.amount_request : st.carry_amount;
                proto.carry_override = true;
                proto.saturation_exempt = true;
                proto.mine_time_mult =
                    hs.time_mult > 0 ? sim::Fixed::from_double(hs.time_mult) : st.mine_time_mult;
                proto.return_delay = st.return_delay;
            }
            b->def.spawn_proto = world.add_spawn_proto(std::move(proto));
        }
        for (const auto& sp : b->tree_spawns)
            if (sp.first >= 0 && sp.first < int32_t(b->tree.size()) && b->tree[std::size_t(sp.first)].spawn_proto < 0)
                b->tree[std::size_t(sp.first)].spawn_proto = world.add_spawn_proto(make_unit(*cat_, sp.second, 0));
        if (!b->tree_salvage.empty()) {
            const data::UnitCost uc = data::unit_cost(*cat_, unit_id);
            for (int32_t k : b->tree_salvage)
                if (k >= 0 && k < int32_t(b->tree.size())) {
                    b->tree[std::size_t(k)].mineral_cost = uc.minerals;
                    b->tree[std::size_t(k)].gas_cost = uc.gas;
                }
        }
        Entry e = book_entry(world, unit_id, *b);
        e.requires_creep = b->spawn_requires_creep;
        e.requires_spawner = b->requires_spawner_target;
        const bool has_tree =
            !b->tree.empty() && b->tree_root >= 0 && b->tree_root < int32_t(b->tree.size());
        const sim::EffectKind kind = has_tree ? b->tree[b->tree_root].kind : b->def.kind;
        e.support = (kind == sim::EffectKind::Heal || kind == sim::EffectKind::CreateHealer ||
                     kind == sim::EffectKind::Inject);
        if (b->from_effect_tree && b->target_rel_authored) {
            e.rel_authored = true;
            e.exc_rel = b->target_exc_rel;
        } else {
            static const bool kRelOld = std::getenv("OMC_CAST_FILTER_REL_OLD") != nullptr;
            bool authored = false;
            const bool harmful = kind == sim::EffectKind::Damage;
            const std::uint8_t m = (kRelOld || harmful) ? 0 : ability_target_excluded_rel(*cat_, abil_id, authored);
            if (authored) {
                e.rel_authored = true;
                e.exc_rel = m;
            }
        }
        return &(ondemand_[key] = std::move(e));
    }

    bool best_point(const sim::World& world, const sim::Unit& caster, const Entry& e, Fixed& out_x,
                    Fixed& out_y) const {
        const auto& units = world.units();
        std::uint32_t best = sim::kNoTarget;
        int best_cov = 0;
        const Fixed r2 = e.aoe_radius * e.aoe_radius;
        for (const sim::Unit& c : units) {
            if (!c.alive || world.are_allied(c.player, caster.player) || c.player < 0) continue;
            const Fixed dx = c.x - caster.x, dy = c.y - caster.y;
            const Fixed reach = e.range + caster.radius + c.radius;
            if (dx * dx + dy * dy > reach * reach) continue;
            int cov = 0;
            for (const sim::Unit& o : units) {
                if (!o.alive || world.are_allied(o.player, caster.player) || o.player < 0) continue;
                const Fixed ex = o.x - c.x, ey = o.y - c.y;
                if (ex * ex + ey * ey <= r2) ++cov;
            }
            if (best == sim::kNoTarget || cov > best_cov) {
                best = c.id;
                best_cov = cov;
                out_x = c.x;
                out_y = c.y;
            }
        }
        return best != sim::kNoTarget;
    }

    std::uint32_t best_target(const sim::World& world, const sim::Unit& caster, const Entry& e) const {
        std::uint32_t best = sim::kNoTarget;
        Fixed best_d2;
        const auto& units = world.units();
        for (const sim::Unit& o : units) {
            if (!o.alive || world.are_allied(o.player, caster.player) || o.player < 0) continue;
            if (o.loaded) continue;
            if ((o.attributes & e.req_attr) != e.req_attr) continue;
            if (o.attributes & e.exc_attr) continue;
            if (e.req_energy && o.energy.raw() <= 0) continue;
            const Fixed dx = o.x - caster.x, dy = o.y - caster.y;
            const Fixed d2 = dx * dx + dy * dy;
            const Fixed reach = e.range + caster.radius + o.radius;
            if (d2 > reach * reach) continue;
            if (best == sim::kNoTarget || d2 < best_d2) {
                best = o.id;
                best_d2 = d2;
            }
        }
        return best;
    }

    const data::Catalog* cat_;
    sim::AbilitySystem abils_;
    std::unordered_map<std::string, Entry> ondemand_;
    std::vector<Entry> entries_;
    std::unordered_map<std::string, std::vector<Entry>> autocast_cache_;
    std::unordered_map<std::string, bool> autocast_toggle_;
    static std::string toggle_key(std::uint32_t unit, const std::string& abil) {
        return std::to_string(unit) + "|" + abil;
    }
};

}
