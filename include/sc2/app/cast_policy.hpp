#pragma once

#include <string>
#include <unordered_map>
#include <vector>

#include "sc2/app/ability.hpp"
#include "sc2/app/spawn.hpp"
#include "sc2/data/catalog.hpp"
#include "sc2/sim/ability_system.hpp"
#include "sc2/sim/world.hpp"

namespace sc2::app {

class CastPolicy {
public:
    using Fixed = sim::Fixed;

    explicit CastPolicy(const data::Catalog& cat) : cat_(&cat) {}

    bool register_offensive(sim::World& world, const std::string& unit_id,
                            const std::string& abil_id) {
        auto b = make_ability(*cat_, abil_id);
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
        entries_.push_back(std::move(e));
        return true;
    }

    bool register_selfcast(sim::World& world, const std::string& unit_id, const std::string& abil_id,
                           Fixed engage_range) {
        auto b = make_ability(*cat_, abil_id);
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
        auto b = make_ability(*cat_, abil_id);
        if (!b) return false;
        const bool has_tree =
            !b->tree.empty() && b->tree_root >= 0 && b->tree_root < int32_t(b->tree.size());
        const sim::EffectKind eff_kind = has_tree ? b->tree[b->tree_root].kind : b->def.kind;
        if (eff_kind != sim::EffectKind::CreateHealer && eff_kind != sim::EffectKind::Heal) return false;
        Entry e = book_entry(world, unit_id, *b);
        e.support = true;
        e.engage_range = range;
        entries_.push_back(std::move(e));
        return true;
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
                bool cast = false;
                if (e.self_cast) {
                    cast = enemy_within(world, u, e.engage_range) && abils_.try_cast(world, ci, e.slot, ci);
                } else if (e.support) {
                    const std::uint32_t tgt = wounded_ally(world, u, e.engage_range);
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
        if (e->support) {
            if (t.player != c.player) return false;
        } else if (t.player == c.player && caster_id != target_id) {
            return false;
        }
        if (e->requires_ground && t.flying) return false;
        const Fixed dx = t.x - c.x, dy = t.y - c.y;
        const Fixed reach = e->range + c.radius + t.radius;
        if (dx * dx + dy * dy > reach * reach) return false;
        return abils_.try_cast(world, caster_id, e->slot, target_id);
    }

    bool cast_now_point(sim::World& world, std::uint32_t caster_id, const std::string& abil_id, Fixed px,
                        Fixed py) {
        const Entry* e = ensure_booked(world, caster_id, abil_id);
        if (!e) return false;
        if (caster_id >= world.units().size()) return false;
        const sim::Unit& c = world.units()[caster_id];
        if (!c.alive) return false;
        if (e->requires_creep) {
            if (!world.creep().is_creep(px, py)) return false;
        } else if (e->range.raw() > 0) {
            const Fixed dx = px - c.x, dy = py - c.y;
            const Fixed reach = e->range + c.radius;
            if (dx * dx + dy * dy > reach * reach) return false;
        }
        return abils_.try_cast_point(world, caster_id, e->slot, px, py);
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
        Fixed engage_range;
        bool requires_creep = false;
        bool requires_ground = false;
    };

    static bool enemy_within(const sim::World& world, const sim::Unit& caster, Fixed range) {
        for (const sim::Unit& c : world.units()) {
            if (!c.alive || c.player == caster.player || c.player < 0) continue;
            const Fixed dx = c.x - caster.x, dy = c.y - caster.y;
            const Fixed reach = range + caster.radius + c.radius;
            if (dx * dx + dy * dy <= reach * reach) return true;
        }
        return false;
    }

    static std::uint32_t wounded_ally(const sim::World& world, const sim::Unit& caster, Fixed range) {
        std::uint32_t best = sim::kNoTarget;
        Fixed best_d2;
        for (const sim::Unit& o : world.units()) {
            if (!o.alive || o.id == caster.id || o.player != caster.player) continue;
            if (o.hp >= o.hp_max) continue;
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

    Entry book_entry(sim::World& world, const std::string& unit_id, const AbilityBuild& b) {
        sim::AbilitySpec spec;
        spec.effect = register_ability(world, b);
        spec.energy_cost = b.energy_cost;
        spec.life_cost = b.life_cost;
        spec.cooldown = b.cooldown;
        spec.req_attr_mask = b.req_attr_mask;
        spec.exc_attr_mask = b.exc_attr_mask;
        spec.require_energy = b.require_energy;
        spec.vital_gates = b.vital_gates;
        Entry e;
        e.unit_id = unit_id;
        e.slot = abils_.add(spec);
        e.range = b.def.range;
        e.req_attr = b.req_attr_mask;
        e.exc_attr = b.exc_attr_mask;
        e.req_energy = b.require_energy;
        e.requires_ground = b.requires_ground;
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
            b->def.spawn_proto = world.add_spawn_proto(std::move(proto));
        }
        Entry e = book_entry(world, unit_id, *b);
        e.requires_creep = b->spawn_requires_creep;
        const bool has_tree =
            !b->tree.empty() && b->tree_root >= 0 && b->tree_root < int32_t(b->tree.size());
        const sim::EffectKind kind = has_tree ? b->tree[b->tree_root].kind : b->def.kind;
        e.support = (kind == sim::EffectKind::Heal || kind == sim::EffectKind::CreateHealer);
        return &(ondemand_[key] = std::move(e));
    }

    bool best_point(const sim::World& world, const sim::Unit& caster, const Entry& e, Fixed& out_x,
                    Fixed& out_y) const {
        const auto& units = world.units();
        std::uint32_t best = sim::kNoTarget;
        int best_cov = 0;
        const Fixed r2 = e.aoe_radius * e.aoe_radius;
        for (const sim::Unit& c : units) {
            if (!c.alive || c.player == caster.player || c.player < 0) continue;
            const Fixed dx = c.x - caster.x, dy = c.y - caster.y;
            const Fixed reach = e.range + caster.radius + c.radius;
            if (dx * dx + dy * dy > reach * reach) continue;
            int cov = 0;
            for (const sim::Unit& o : units) {
                if (!o.alive || o.player == caster.player || o.player < 0) continue;
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
            if (!o.alive || o.player == caster.player || o.player < 0) continue;
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
};

}
