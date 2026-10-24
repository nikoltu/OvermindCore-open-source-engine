#include "sc2/ai/melee_ai.hpp"

#include <utility>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "sc2/ai/defense.hpp"
#include "sc2/ai/economy.hpp"
#include "sc2/ai/scout.hpp"

namespace sc2::ai {

using sim::Fixed;
using sim::Order;
using sim::OrderType;
using sim::Unit;

MeleeAI::MeleeAI(int player, MeleeConfig cfg)
    : player_(player), cfg_(std::move(cfg)) {}

void MeleeAI::set_base(MeleeBase base) { base_ = std::move(base); }

bool MeleeAI::is_worker(uint32_t id) const {
    for (uint32_t w : base_.workers)
        if (w == id) return true;
    return false;
}

bool MeleeAI::is_structure(uint32_t id) const {
    if (id == base_.producer || id == base_.depot) return true;
    for (uint32_t s : base_.structures)
        if (s == id) return true;
    return false;
}

bool MeleeAI::is_army(const Unit& u) const {
    return u.alive && u.player == player_ && u.speed.raw() > 0 && !u.is_worker &&
           !is_worker(u.id) && !is_structure(u.id);
}

int MeleeAI::army_size(const sim::World& w) const {
    int n = 0;
    for (const Unit& u : w.units())
        if (is_army(u)) ++n;
    return n;
}

bool MeleeAI::afford(const sim::World& w, const BuildItem& it) const {
    const sim::Player& p = w.player(player_);
    const int n = it.count > 1 ? it.count : 1;
    return p.minerals >= it.minerals * n && p.gas >= it.gas * n;
}

bool MeleeAI::supply_ok(const sim::World& w, const BuildItem& it) const {
    const int n = it.count > 1 ? it.count : 1;
    const sim::Fixed need = it.proto.supply_uses * sim::Fixed::from_int(n);
    if (need.raw() <= 0) return true;
    return (w.supply_used(player_) + need) <= w.supply_max(player_);
}

int MeleeAI::worker_count(const sim::World& w) const {
    int n = 0;
    for (const Unit& u : w.units())
        if (u.alive && u.player == player_ && u.is_worker) ++n;
    return n;
}

void MeleeAI::run_workers(sim::World& w) {
    const std::size_t U = w.units().size();
    auto live_node = [&](uint32_t nid) {
        return nid < U && w.units()[nid].alive && w.units()[nid].resource_left > 0;
    };
    for (uint32_t id = 0; id < U; ++id) {
        Unit& u = w.unit(id);
        if (!u.alive || u.player != player_ || !u.is_worker) continue;
        if (id == scout_) continue;
        if (w.worker_committed(id)) continue;
        if (u.orders.empty()) {
            if (!live_node(u.gather_target)) {
                u.home_depot = base_.depot;
                u.gather_target = base_.mineral;
            }
            w.issue(id, Order{OrderType::Gather, Fixed(), Fixed(), sim::kNoTarget});
        }
    }
}

const bool MeleeAI::kSupplyAfterProducer = std::getenv("SC2_AI_SUPPLY_AFTER_PRODUCER") != nullptr;

int MeleeAI::own_pending(const sim::World& w) const {
    static const bool kWorld = std::getenv("SC2_AI_WORLD_PENDING") != nullptr;
    return int(kWorld ? w.pending_count() : w.pending_count(player_));
}

bool MeleeAI::run_supply(sim::World& w) {
    if (!cfg_.has_supply || kSupplyAfterProducer) return false;
    const Fixed used = w.supply_used(player_), cap = w.supply_max(player_);
    if (cap >= Fixed::from_int(200) || used + cfg_.supply_margin < cap) return false;
    const std::string& sid = cfg_.supply_unit.proto.catalog_id;
    for (const Unit& u : w.units())
        if (u.alive && u.player == player_ && u.constructing && u.supply_provides.raw() > 0) return false;
    for (const SiteBuild& s : site_builds_)
        if (s.id == sid) return false;
    if (!sid.empty() && w.pending_of_type(player_, sid) > 0) return false;
    if (!afford(w, cfg_.supply_unit)) return false;
    const bool morph_at_hall = base_.depot < w.units().size() && w.unit(base_.depot).spawner_proto >= 0 &&
                               cfg_.supply_unit.proto.speed.raw() > 0;
    const bool has_producer = base_.producer < w.units().size() && w.unit(base_.producer).alive;
    return make_item(w, morph_at_hall || !has_producer ? base_.depot : base_.producer, cfg_.supply_unit);
}

void MeleeAI::run_production(sim::World& w) {
    if (run_supply(w)) return;

    if (cfg_.worker_target > 0 && base_.depot < w.units().size() &&
        w.unit(base_.depot).alive && worker_count(w) < cfg_.worker_target &&
        own_pending(w) < cfg_.max_pending && afford(w, cfg_.worker_unit) &&
        supply_ok(w, cfg_.worker_unit)) {
        uint32_t hall = base_.depot;
        Unit wp = cfg_.worker_unit.proto;
        if (base_.expansion < w.units().size() && w.unit(base_.expansion).alive &&
            base_.expansion_mineral < w.units().size() &&
            w.unit(base_.expansion_mineral).alive &&
            w.unit(base_.expansion_mineral).resource_left > 0) {
            hall = base_.expansion;
            wp.home_depot = base_.expansion;
            wp.gather_target = base_.expansion_mineral;
        }
        if (cfg_.worker_train_delay_ticks <= 0 || w.tick() - last_worker_train_tick_ >= cfg_.worker_train_delay_ticks) {
            if (w.queue_train(hall, wp, cfg_.worker_unit.minerals, cfg_.worker_unit.gas, cfg_.worker_unit.build_time))
                last_worker_train_tick_ = w.tick();
        }
    }

    if (base_.producer >= w.units().size() || !w.unit(base_.producer).alive) {
        base_.producer = sim::kNoTarget;
        if (producer_pending_) {
            for (const Unit& u : w.units()) {
                if (u.alive && u.player == player_ && u.speed.raw() == 0 && !u.constructing &&
                    (producer_id_.empty() || u.catalog_id == producer_id_) &&
                    u.id != base_.depot) {
                    base_.producer = u.id;
                    producer_pending_ = false;
                    break;
                }
            }
        }
        if (base_.producer == sim::kNoTarget) {
            if (cfg_.has_producer && !producer_pending_ && !stock_active() &&
                base_.depot < w.units().size() && afford(w, cfg_.producer_unit)) {
                if (make_item(w, base_.depot, cfg_.producer_unit)) {
                    producer_pending_ = true;
                    if (cfg_.worker_builds) producer_id_ = cfg_.producer_unit.proto.catalog_id;
                }
            }
            return;
        }
    }
    if (own_pending(w) >= cfg_.max_pending) return;
    if (stock_active()) return;

    if (cfg_.has_army_unit2 && !cfg_.tech_chain.empty() && base_.depot < w.units().size() &&
        w.unit(base_.depot).alive) {
        for (const BuildItem& tb : cfg_.tech_chain) {
            if (tb.addon_self) {
                const uint32_t prod = base_.producer;
                if (prod >= w.units().size() || !w.unit(prod).alive) break;
                if (w.addon_of(prod) == sim::World::kAddonTechLab) continue;
                if (w.unit(prod).addon_building != 0) return;
                if (!afford(w, tb)) break;
                w.player(player_).minerals -= tb.minerals;
                w.player(player_).gas -= tb.gas;
                w.build_addon(prod, sim::World::kAddonTechLab, tb.build_time);
                return;
            }
            bool have = false;
            for (const Unit& u : w.units())
                if (u.alive && u.player == player_ && u.catalog_id == tb.proto.catalog_id) { have = true; break; }
            if (have) continue;
            if (tb.morph_self) {
                if (w.is_morphing(base_.depot)) return;
                if (!afford(w, tb)) break;
                w.player(player_).minerals -= tb.minerals;
                w.player(player_).gas -= tb.gas;
                const int32_t idx = w.add_spawn_proto(tb.proto);
                w.begin_morph(base_.depot, idx, tb.build_time);
                return;
            }
            if (afford(w, tb) && make_item(w, base_.depot, tb))
                return;
            break;
        }
    }

    if (cfg_.has_supply && kSupplyAfterProducer) {
        Fixed used = w.supply_used(player_), cap = w.supply_max(player_);
        if (cap < Fixed::from_int(200) && used + cfg_.supply_margin >= cap &&
            afford(w, cfg_.supply_unit)) {
            const bool morph_at_hall = base_.depot < w.units().size() &&
                                       w.unit(base_.depot).spawner_proto >= 0 &&
                                       cfg_.supply_unit.proto.speed.raw() > 0;
            make_item(w, morph_at_hall ? base_.depot : base_.producer, cfg_.supply_unit);
            return;
        }
    }

    const bool in_order = build_idx_ < cfg_.build_order.size();
    const bool want2 = cfg_.has_army_unit2 && army_alt_ && army_unit2_gate_ok(w);
    const BuildItem& item =
        in_order ? cfg_.build_order[build_idx_]
                 : (want2 ? cfg_.army_unit2 : cfg_.army_unit);
    if (afford(w, item) && supply_ok(w, item)) {
        const bool morph_at_hall = base_.depot < w.units().size() &&
                                   w.unit(base_.depot).spawner_proto >= 0 &&
                                   item.proto.speed.raw() > 0;
        const uint32_t train_from = morph_at_hall ? base_.depot : base_.producer;
        if (make_item(w, train_from, item)) {
            if (in_order) ++build_idx_;
            else if (cfg_.has_army_unit2) army_alt_ = !army_alt_;
        }
    }
}

bool MeleeAI::army_unit2_gate_ok(const sim::World& w) const {
    if (cfg_.army_unit2_gate.empty()) return true;
    for (const Unit& u : w.units())
        if (u.alive && u.player == player_ && u.catalog_id == cfg_.army_unit2_gate) return true;
    return false;
}

int32_t MeleeAI::target_player(const sim::World& w, bool* several) const {
    int32_t best_player = -1, first = -1;
    std::int64_t best = -1;
    if (several) *several = false;
    for (const Unit& u : w.units()) {
        if (!u.alive || u.player < 0 || u.player == player_) continue;
        if (w.are_allied(u.player, player_) || w.player(u.player).defeated) continue;
        if (first < 0) first = u.player;
        else if (several && u.player != first) *several = true;
        const std::int64_t dx = std::int64_t(u.x.raw()) - cfg_.enemy_x.raw(), dy = std::int64_t(u.y.raw()) - cfg_.enemy_y.raw();
        const std::int64_t d2 = dx * dx + dy * dy;
        if (best < 0 || d2 < best) {
            best = d2;
            best_player = u.player;
        }
    }
    return best_player;
}

bool MeleeAI::enemy_target(const sim::World& w, Fixed& tx, Fixed& ty) const {
    Fixed sx, sy;
    int n = 0;
    bool several = false;
    const int32_t foe = cfg_.match_players > 2 ? target_player(w, &several) : -1;
    for (const Unit& u : w.units()) {
        if (!u.alive || u.player < 0 || u.player == player_) continue;
        if (w.are_allied(u.player, player_) || w.player(u.player).defeated || (several && u.player != foe)) continue;
        sx = sx + u.x;
        sy = sy + u.y;
        ++n;
    }
    if (n == 0) return false;
    Fixed inv = Fixed::from_int(n);
    tx = sx / inv;
    ty = sy / inv;
    return true;
}

void MeleeAI::run_army(sim::World& w) {
    const int count = army_size(w);
    int desired = cfg_.army_threshold;
    int launch_at = cfg_.army_threshold;
    if (!cfg_.wave_phases.empty()) {
        const MeleeConfig::WavePhase& ph = cfg_.wave_phases[std::min(wave_phase_, cfg_.wave_phases.size() - 1)];
        desired = ph.desired_army;
        if (wave_phase_start_tick_ < 0) wave_phase_start_tick_ = w.tick();
        const bool elapsed = w.tick() - wave_phase_start_tick_ >= ph.attack_duration_s * sim::World::kTicksPerSecond;
        launch_at = elapsed ? std::max(1, desired / 2) : desired;
        const int since_combat = w.tick() - w.player(player_).last_combat_tick;
        if (count > 0 && since_combat >= (600 - 2 * count) * sim::World::kTicksPerSecond) launch_at = std::min(launch_at, count);
    }
    if (!attacking_ && count >= launch_at) {
        int32_t enemy = cfg_.match_players > 2 ? target_player(w) : -1;
        if (cfg_.match_players <= 2) {
            for (const Unit& u : w.units())
                if (u.alive && u.player >= 0 && u.player != player_ && !w.are_allied(u.player, player_) &&
                    !w.player(u.player).defeated) {
                    enemy = u.player;
                    break;
                }
        }
        const Fixed mine = army_power(w, player_);
        const Fixed theirs = enemy >= 0 ? army_power(w, enemy) : Fixed();
        if (should_engage(mine, theirs, cfg_.attack_caution)) {
            attacking_ = true;
            launched_desired_ = desired;
            if (!cfg_.wave_phases.empty()) {
                if (wave_phase_ + 1 < cfg_.wave_phases.size()) ++wave_phase_;
                wave_phase_start_tick_ = w.tick();
            }
        }
    } else if (attacking_ && count < std::max(1, (launched_desired_ > 0 ? launched_desired_ : desired) / 2)) {
        attacking_ = false;
    }

    Fixed tx = cfg_.enemy_x, ty = cfg_.enemy_y;
    if (!enemy_target(w, tx, ty)) {
        tx = cfg_.enemy_x;
        ty = cfg_.enemy_y;
    }

    auto needs = [](const Unit& u, OrderType want) {
        return u.orders.empty() || u.orders.front().type != want;
    };

    if (cfg_.smart_combat && attacking_) {
        combat_.retreat_hp_frac = cfg_.flee_damage ? Fixed::from_ratio(1, 3) : Fixed();
        std::vector<uint32_t> army;
        for (const Unit& u : w.units())
            if (is_army(u)) army.push_back(u.id);

        Fixed adv_x = tx, adv_y = ty;
        uint32_t threat_enemy = sim::kNoTarget;
        const uint32_t base = threatened_base(w, player_, cfg_.defense_radius, threat_enemy);
        if (base != sim::kNoTarget) {
            adv_x = w.unit(base).x;
            adv_y = w.unit(base).y;
        }

        if (combat_.command(w, army, player_, cfg_.rally_x, cfg_.rally_y) ==
            CombatController::Stance::Idle) {
            for (uint32_t id : army)
                if (needs(w.unit(id), OrderType::AttackMove))
                    w.issue(id, Order{OrderType::AttackMove, adv_x, adv_y, sim::kNoTarget});
        }
        return;
    }

    for (const Unit& u : w.units()) {
        if (!is_army(u)) continue;
        const uint32_t id = u.id;
        if (attacking_) {
            if (needs(u, OrderType::AttackMove))
                w.issue(id, Order{OrderType::AttackMove, tx, ty, sim::kNoTarget});
            continue;
        }
        Fixed dx = cfg_.rally_x - u.x, dy = cfg_.rally_y - u.y;
        Fixed d2 = dx * dx + dy * dy;
        Fixed arrive = u.radius + Fixed::from_int(1);
        if (d2 <= arrive * arrive) {
            if (needs(u, OrderType::HoldPosition))
                w.issue(id, Order{OrderType::HoldPosition, Fixed(), Fixed(),
                                  sim::kNoTarget});
        } else if (needs(u, OrderType::Move)) {
            w.issue(id, Order{OrderType::Move, cfg_.rally_x, cfg_.rally_y,
                              sim::kNoTarget});
        }
    }
}

int MeleeAI::patches_near(const sim::World& w, Fixed x, Fixed y, Fixed radius) const {
    const Fixed r2 = radius * radius;
    int n = 0;
    for (const Unit& u : w.units()) {
        if (!u.alive || u.player >= 0 || u.resource_left <= 0) continue;
        const Fixed dx = u.x - x, dy = u.y - y;
        if (dx * dx + dy * dy <= r2) ++n;
    }
    return n;
}

uint32_t MeleeAI::nearest_mineral(const sim::World& w, Fixed x, Fixed y) const {
    uint32_t best = sim::kNoTarget;
    Fixed best_d2;
    for (const Unit& u : w.units()) {
        if (!u.alive || u.player >= 0 || u.resource_left <= 0) continue;
        const Fixed dx = u.x - x, dy = u.y - y, d2 = dx * dx + dy * dy;
        if (best == sim::kNoTarget || d2 < best_d2) {
            best = u.id;
            best_d2 = d2;
        }
    }
    return best;
}

void MeleeAI::run_expansion(sim::World& w) {
    if (!cfg_.has_expansion) return;
    const std::size_t U = w.units().size();

    if (expansion_pending_ && base_.expansion == sim::kNoTarget) {
        const Fixed tol = Fixed::from_int(3);
        for (const Unit& u : w.units()) {
            if (!u.alive || u.player != player_ || u.speed.raw() != 0) continue;
            if (u.id == base_.depot || u.id == base_.producer) continue;
            const Fixed dx = u.x - expand_x_, dy = u.y - expand_y_;
            if (dx * dx + dy * dy <= tol * tol) {
                base_.expansion = u.id;
                base_.expansion_mineral = nearest_mineral(w, u.x, u.y);
                expansion_pending_ = false;
                cfg_.worker_target += cfg_.expansion_worker_bonus;
                break;
            }
        }
    }

    if (base_.expansion != sim::kNoTarget || expansion_pending_) return;
    if (base_.depot >= U || !w.unit(base_.depot).alive) return;
    const Unit& depot = w.unit(base_.depot);

    const int patches =
        patches_near(w, depot.x, depot.y, cfg_.expansion_occupied_radius + Fixed::from_int(4));
    if (!should_expand(worker_count(w), patches, w.player(player_).minerals,
                       cfg_.expansion_unit.minerals))
        return;

    std::vector<std::pair<Fixed, Fixed>> bases;
    for (const Unit& u : w.units())
        if (u.alive && u.speed.raw() == 0 && u.supply_provides.raw() > 0)
            bases.push_back({u.x, u.y});

    const int idx = nearest_expansion(cfg_.expansion_sites, bases, depot.x, depot.y,
                                      cfg_.expansion_occupied_radius);
    if (idx < 0) return;
    const auto& site = cfg_.expansion_sites[std::size_t(idx)];
    if (w.queue_train_at(base_.depot, cfg_.expansion_unit.proto, cfg_.expansion_unit.minerals,
                         cfg_.expansion_unit.gas, cfg_.expansion_unit.build_time, site.first,
                         site.second)) {
        expansion_pending_ = true;
        expand_x_ = site.first;
        expand_y_ = site.second;
    }
}

int MeleeAI::owned_or_pending(const sim::World& w, const std::string& id) const {
    int n = 0;
    for (const Unit& u : w.units())
        if (u.alive && u.player == player_ && u.catalog_id == id) ++n;
    for (const SiteBuild& sb : site_builds_)
        if (sb.id == id) ++n;
    return n + w.pending_of_type(player_, id);
}

void MeleeAI::prune_site_builds(const sim::World& w) {
    for (std::size_t i = 0; i < site_builds_.size();) {
        const uint32_t b = site_builds_[i].builder;
        const bool live = b < w.units().size() && w.units()[b].alive && w.worker_committed(b) &&
                          w.units()[b].construct_target == sim::kNoTarget;
        if (live) ++i;
        else site_builds_.erase(site_builds_.begin() + long(i));
    }
}

bool MeleeAI::make_item(sim::World& w, uint32_t from, const BuildItem& item) {
    if (cfg_.worker_builds && item.proto.speed.raw() == 0 && !item.morph_self && !item.addon_self)
        return place_structure(w, item);
    if (w.queue_train(from, item.proto, item.minerals, item.gas, item.build_time, item.count)) return true;
    if (!cfg_.worker_builds || item.proto.speed.raw() == 0 || from == base_.depot) return false;
    const std::size_t U = w.units().size();
    std::vector<std::pair<int32_t, uint32_t>> others;
    for (uint32_t id = 0; id < U; ++id) {
        const Unit& u = w.unit(id);
        if (!u.alive || u.player != player_ || u.speed.raw() != 0 || u.constructing || u.flying) continue;
        if (id == from || id == base_.depot || u.is_gas || u.addon_body_kind != 0) continue;
        others.push_back({w.queue_count(id), id});
    }
    std::sort(others.begin(), others.end());
    for (const std::pair<int32_t, uint32_t>& o : others)
        if (w.queue_train(o.second, item.proto, item.minerals, item.gas, item.build_time, item.count)) return true;
    return false;
}

bool MeleeAI::place_structure(sim::World& w, const BuildItem& item) {
    const std::size_t U = w.units().size();
    if (base_.depot >= U || !w.unit(base_.depot).alive) return false;
    const Fixed hx = w.unit(base_.depot).x, hy = w.unit(base_.depot).y;
    auto nearest_builder = [&](Fixed x, Fixed y) {
        uint32_t best = sim::kNoTarget;
        Fixed best_d2;
        for (uint32_t id = 0; id < U; ++id) {
            const Unit& u = w.unit(id);
            if (!u.alive || u.player != player_ || !u.is_worker || u.loaded || id == scout_) continue;
            if (w.worker_committed(id)) continue;
            const Fixed dx = u.x - x, dy = u.y - y;
            const Fixed d2 = dx * dx + dy * dy;
            if (best == sim::kNoTarget || d2 < best_d2) { best = id; best_d2 = d2; }
        }
        return best;
    };
    if (item.proto.requires_vespene) {
        uint32_t node = sim::kNoTarget;
        Fixed node_d2;
        for (uint32_t id = 0; id < U; ++id) {
            const Unit& g = w.unit(id);
            if (!g.alive || !g.is_gas || g.resource_left <= 0 || g.player >= 0) continue;
            const Fixed dx = g.x - hx, dy = g.y - hy;
            const Fixed d2 = dx * dx + dy * dy;
            if (d2 > Fixed::from_int(14 * 14)) continue;
            if (node == sim::kNoTarget || d2 < node_d2) { node = id; node_d2 = d2; }
        }
        if (node == sim::kNoTarget) return false;
        const Fixed gx = w.unit(node).x, gy = w.unit(node).y;
        const uint32_t builder = nearest_builder(gx, gy);
        if (builder == sim::kNoTarget) return false;
        Unit proto = item.proto;
        proto.player = player_;
        proto.is_gas = true;
        proto.resource_left = w.unit(node).resource_left;
        if (!w.issue_worker_build(builder, gx, gy, proto, item.minerals, item.gas, item.build_time,
                                  Fixed::from_int(2), w.unit(builder).build_verb))
            return false;
        w.unit(node).resource_left = 0;
        site_builds_.push_back({item.proto.catalog_id, builder});
        return true;
    }
    if (item.proto.needs_power && cfg_.has_supply && cfg_.supply_unit.proto.power_radius.raw() > 0 &&
        !std::getenv("SC2_AI_NO_POWER_FIRST")) {
        bool own_field = false;
        for (const Unit& o : w.units())
            if (o.alive && o.player == player_ && o.power_radius.raw() > 0) { own_field = true; break; }
        if (!own_field) {
            const bool placed = afford(w, cfg_.supply_unit) && place_structure(w, cfg_.supply_unit);
            if (std::getenv("SC2_AI_TRACE"))
                std::fprintf(stderr, "[ai] p%d tick %d %s needs power: no field - supply first %s\n", int(player_), int(w.tick()),
                             item.proto.catalog_id.c_str(), placed ? "PLACED" : "not placed");
            return false;
        }
    }
    Fixed mx, my;
    bool has_mineral = false;
    if (base_.mineral < U && w.unit(base_.mineral).alive) {
        mx = w.unit(base_.mineral).x - hx;
        my = w.unit(base_.mineral).y - hy;
        has_mineral = true;
    }
    const uint32_t builder = nearest_builder(hx, hy);
    if (builder == sim::kNoTarget) return false;
    Unit proto = item.proto;
    proto.player = player_;
    const Fixed item_h = item.proto.footprint_offsets.empty() ? item.proto.radius : w.site_half_extent(item.proto);
    auto boxes_meet = [](Fixed ax, Fixed ay, Fixed ah, Fixed bx, Fixed by, Fixed bh) {
        const Fixed dx = ax > bx ? ax - bx : bx - ax, dy = ay > by ? ay - by : by - ay;
        return dx < ah + bh && dy < ah + bh;
    };
    auto slot_conflict = [&](Fixed x, Fixed y) {
        const Fixed one = Fixed::from_int(1);
        for (const Unit& o : w.units()) {
            if (!o.alive || o.speed.raw() != 0 || o.flying) continue;
            if (o.player == player_ && o.addon_slot_host) {
                Fixed sx, sy;
                sim::World::addon_slot(o.x, o.y, sx, sy);
                if (boxes_meet(x, y, item_h, sx, sy, one)) return true;
            }
            if (item.proto.addon_slot_host) {
                Fixed sx, sy;
                sim::World::addon_slot(x, y, sx, sy);
                if (boxes_meet(sx, sy, one, o.x, o.y, w.site_half_extent(o))) return true;
            }
        }
        return false;
    };
    static const bool kAnyLevel = std::getenv("SC2_AI_ANY_LEVEL_SITE") != nullptr;
    const sim::NavGrid& nav = w.nav();
    int hall_level = -1;
    if (!kAnyLevel && nav.configured() && nav.has_levels()) {
        int cx = 0, cy = 0;
        nav.to_cell(hx, hy, cx, cy);
        hall_level = nav.level_cell(cx, cy);
    }
    const int r0 = int((w.unit(base_.depot).radius + item.proto.radius).to_double()) + 2;
    for (int r = r0; r <= r0 + 14; ++r) {
        for (int dy = -r; dy <= r; ++dy)
            for (int dx = -r; dx <= r; ++dx) {
                if (dx != -r && dx != r && dy != -r && dy != r) continue;
                if ((dx & 1) || (dy & 1)) continue;
                if (has_mineral && (Fixed::from_int(dx) * mx + Fixed::from_int(dy) * my).raw() > 0) continue;
                const Fixed x = hx + Fixed::from_int(dx), y = hy + Fixed::from_int(dy);
                if (x.raw() <= 0 || y.raw() <= 0) continue;
                if (hall_level >= 0) {
                    int cx = 0, cy = 0;
                    nav.to_cell(x, y, cx, cy);
                    if (nav.level_cell(cx, cy) != hall_level) continue;
                }
                if (slot_conflict(x, y)) continue;
                if (w.issue_worker_build(builder, x, y, proto, item.minerals, item.gas, item.build_time,
                                         Fixed::from_int(2), w.unit(builder).build_verb)) {
                    site_builds_.push_back({item.proto.catalog_id, builder});
                    return true;
                }
            }
    }
    if (item.proto.needs_power && w.last_error() == sim::CmdError::CantBuildTooFarFromBuildPowerSource && cfg_.has_supply &&
        cfg_.supply_unit.proto.power_radius.raw() > 0 && !std::getenv("SC2_AI_NO_POWER_FIRST")) {
        bool rising = false;
        for (const Unit& o : w.units())
            if (o.alive && o.player == player_ && o.power_radius.raw() > 0 && o.constructing) { rising = true; break; }
        if (!rising && afford(w, cfg_.supply_unit)) place_structure(w, cfg_.supply_unit);
    }
    if (std::getenv("SC2_AI_TRACE"))
        std::fprintf(stderr, "[ai] p%d tick %d %s: no site accepted (last refusal %s, minerals %d)\n", int(player_), int(w.tick()),
                     item.proto.catalog_id.c_str(), sim::cmd_error_key(w.last_error()), int(w.player(player_).minerals));
    return false;
}

void MeleeAI::run_gas(sim::World& w) {
    if (!cfg_.worker_builds || cfg_.gas_peons_per_geyser <= 0) return;
    const std::size_t U = w.units().size();
    for (uint32_t g = 0; g < U; ++g) {
        const Unit& gs = w.unit(g);
        if (!gs.alive || gs.player != player_ || !gs.is_gas || gs.constructing || gs.resource_left <= 0) continue;
        int crew = 0;
        for (const Unit& u : w.units())
            if (u.alive && u.player == player_ && u.is_worker && u.gathers_gas && u.gather_target == g) ++crew;
        for (uint32_t id = 0; id < U && crew < cfg_.gas_peons_per_geyser; ++id) {
            Unit& u = w.unit(id);
            if (!u.alive || u.player != player_ || !u.is_worker || u.gathers_gas || u.loaded) continue;
            if (id == scout_ || w.worker_committed(id)) continue;
            u.gathers_gas = true;
            u.home_depot = base_.depot;
            u.gather_target = g;
            w.issue(id, Order{OrderType::Gather, Fixed(), Fixed(), sim::kNoTarget});
            ++crew;
        }
    }
}

void MeleeAI::run_stock(sim::World& w) {
    const std::size_t U = w.units().size();
    while (stock_idx_ < cfg_.stock.size()) {
        const StockStep& s = cfg_.stock[stock_idx_];
        if (s.kind == StockKind::Skip) { ++stock_idx_; continue; }
        if (s.kind == StockKind::Worker) {
            if (cfg_.worker_target < s.count) cfg_.worker_target = s.count;
            if (worker_count(w) + w.pending_of_type(player_, s.id) >= s.count) { ++stock_idx_; continue; }
            return;
        }
        if (owned_or_pending(w, s.id) >= s.count) { ++stock_idx_; continue; }
        if (own_pending(w) >= cfg_.max_pending) return;
        if (!afford(w, s.item) || !supply_ok(w, s.item)) return;
        const bool hall_ok = base_.depot < U && w.unit(base_.depot).alive;
        const bool prod_ok = base_.producer < U && w.unit(base_.producer).alive;
        const bool larva_hall = hall_ok && w.unit(base_.depot).spawner_proto >= 0;
        uint32_t from = sim::kNoTarget;
        if (s.kind == StockKind::Structure || s.kind == StockKind::Producer) from = hall_ok ? base_.depot : sim::kNoTarget;
        else if (s.kind == StockKind::Supply) from = s.item.proto.speed.raw() > 0 && larva_hall ? base_.depot : (hall_ok ? base_.depot : sim::kNoTarget);
        else if (s.kind == StockKind::Army) from = larva_hall && s.item.proto.speed.raw() > 0 ? base_.depot : (prod_ok ? base_.producer : sim::kNoTarget);
        if (from == sim::kNoTarget) return;
        if (make_item(w, from, s.item) && s.kind == StockKind::Producer && base_.producer == sim::kNoTarget) {
            producer_pending_ = true;
            if (cfg_.worker_builds) producer_id_ = s.item.proto.catalog_id;
        }
        return;
    }
}

void MeleeAI::run_scout(sim::World& w) {
    if (cfg_.num_scouts <= 0 || scout_done_) return;
    const std::size_t U = w.units().size();
    if (scout_ != sim::kNoTarget && (scout_ >= U || !w.unit(scout_).alive)) {
        scout_done_ = true;
        scout_ = sim::kNoTarget;
        return;
    }
    if (scout_ == sim::kNoTarget) {
        for (uint32_t id = 0; id < U; ++id) {
            const Unit& u = w.unit(id);
            if (u.alive && u.player == player_ && u.is_worker) { scout_ = id; break; }
        }
        if (scout_ == sim::kNoTarget) return;
        const Unit& sc = w.unit(scout_);
        Fixed lx = cfg_.enemy_x, ly = cfg_.enemy_y;
        {
            const Fixed dx = sc.x - cfg_.enemy_x, dy = sc.y - cfg_.enemy_y;
            const Fixed d = sim::isqrt(dx * dx + dy * dy);
            if (d.raw() > 0) {
                lx = cfg_.enemy_x + dx * Fixed::from_int(6) / d;
                ly = cfg_.enemy_y + dy * Fixed::from_int(6) / d;
            }
        }
        w.issue(scout_, Order{OrderType::Move, lx, ly, sim::kNoTarget});
        scout_at_start_ = true;
        {
            const Fixed dx = lx - sc.x, dy = ly - sc.y;
            const Fixed d = sim::isqrt(dx * dx + dy * dy);
            const int walk_ticks = sc.speed.raw() > 0 ? int((d / sc.speed).to_double() * sim::World::kTicksPerSecond) : 0;
            scout_leg_deadline_ = w.tick() + 2 * walk_ticks + 2 * sim::World::kTicksPerSecond;
        }
        return;
    }
    const Unit& s = w.unit(scout_);
    bool seen = false;
    if (scout_at_start_ && !s.orders.empty()) {
        const Fixed sight = s.sight.raw() > 0 ? s.sight : Fixed::from_int(8);
        const Fixed dx = cfg_.enemy_x - s.x, dy = cfg_.enemy_y - s.y;
        seen = dx * dx + dy * dy <= sight * sight;
    }
    bool stalled = false;
    if (!s.orders.empty()) {
        const Fixed dx = s.orders.front().tx - s.x, dy = s.orders.front().ty - s.y;
        const Fixed d2 = dx * dx + dy * dy;
        if (scout_best_d2_.raw() < 0 || d2 < scout_best_d2_) {
            scout_best_d2_ = d2;
            scout_no_progress_ = 0;
        } else if (++scout_no_progress_ >= 8) {
            stalled = true;
        }
        if (w.tick() >= scout_leg_deadline_) stalled = true;
    }
    if (!s.orders.empty() && !stalled && !seen) return;
    if (stalled || seen) w.stop(scout_);
    scout_best_d2_ = Fixed::from_int(-1);
    scout_no_progress_ = 0;
    if (s.is_worker || !has_bounds_) {
        scout_done_ = true;
        scout_ = sim::kNoTarget;
        return;
    }
    vision_.compute(w, player_, Fixed::from_int(10), Fixed::from_int(2), bounds_,
                    w.placement().configured() ? &w.placement() : nullptr);
    Fixed tx, ty;
    if (scout_target(vision_, bounds_, Fixed::from_int(2), s.x, s.y, tx, ty)) {
        const Fixed dx = tx - s.x, dy = ty - s.y;
        const Fixed d = sim::isqrt(dx * dx + dy * dy);
        const int walk_ticks = s.speed.raw() > 0 ? int((d / s.speed).to_double() * sim::World::kTicksPerSecond) : 0;
        scout_leg_deadline_ = w.tick() + 2 * walk_ticks + 2 * sim::World::kTicksPerSecond;
        w.issue(scout_, Order{OrderType::Move, tx, ty, sim::kNoTarget});
    } else {
        scout_done_ = true;
        scout_ = sim::kNoTarget;
    }
}

void MeleeAI::update(sim::World& w) {
    run_scout(w);
    prune_site_builds(w);
    run_workers(w);
    run_gas(w);
    run_stock(w);
    run_production(w);
    run_expansion(w);
    run_army(w);
}

}
