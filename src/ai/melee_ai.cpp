#include "sc2/ai/melee_ai.hpp"

#include <utility>
#include <algorithm>
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
        if (u.orders.empty()) {
            if (!live_node(u.gather_target)) {
                u.home_depot = base_.depot;
                u.gather_target = base_.mineral;
            }
            w.issue(id, Order{OrderType::Gather, Fixed(), Fixed(), sim::kNoTarget});
        }
    }
}

void MeleeAI::run_production(sim::World& w) {
    if (cfg_.worker_target > 0 && base_.depot < w.units().size() &&
        w.unit(base_.depot).alive && worker_count(w) < cfg_.worker_target &&
        int(w.pending_count()) < cfg_.max_pending && afford(w, cfg_.worker_unit) &&
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
                if (u.alive && u.player == player_ && u.speed.raw() == 0 &&
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
                if (w.queue_train(base_.depot, cfg_.producer_unit.proto,
                                  cfg_.producer_unit.minerals, cfg_.producer_unit.gas,
                                  cfg_.producer_unit.build_time))
                    producer_pending_ = true;
            }
            return;
        }
    }
    if (int(w.pending_count()) >= cfg_.max_pending) return;
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
            if (afford(w, tb) &&
                w.queue_train(base_.depot, tb.proto, tb.minerals, tb.gas, tb.build_time))
                return;
            break;
        }
    }

    if (cfg_.has_supply) {
        Fixed used = w.supply_used(player_), cap = w.supply_max(player_);
        if (cap < Fixed::from_int(200) && used + cfg_.supply_margin >= cap &&
            afford(w, cfg_.supply_unit)) {
            const bool morph_at_hall = base_.depot < w.units().size() &&
                                       w.unit(base_.depot).spawner_proto >= 0 &&
                                       cfg_.supply_unit.proto.speed.raw() > 0;
            w.queue_train(morph_at_hall ? base_.depot : base_.producer, cfg_.supply_unit.proto,
                          cfg_.supply_unit.minerals, cfg_.supply_unit.gas,
                          cfg_.supply_unit.build_time);
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
        if (w.queue_train(train_from, item.proto, item.minerals, item.gas, item.build_time, item.count)) {
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

bool MeleeAI::enemy_target(const sim::World& w, Fixed& tx, Fixed& ty) const {
    Fixed sx, sy;
    int n = 0;
    for (const Unit& u : w.units()) {
        if (!u.alive || u.player < 0 || u.player == player_) continue;
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
    }
    if (!attacking_ && count >= launch_at) {
        int32_t enemy = -1;
        for (const Unit& u : w.units())
            if (u.alive && u.player >= 0 && u.player != player_) { enemy = u.player; break; }
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
    return n + w.pending_of_type(player_, id);
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
        if (int(w.pending_count()) >= cfg_.max_pending) return;
        if (!afford(w, s.item) || !supply_ok(w, s.item)) return;
        const bool hall_ok = base_.depot < U && w.unit(base_.depot).alive;
        const bool prod_ok = base_.producer < U && w.unit(base_.producer).alive;
        const bool larva_hall = hall_ok && w.unit(base_.depot).spawner_proto >= 0;
        uint32_t from = sim::kNoTarget;
        if (s.kind == StockKind::Structure || s.kind == StockKind::Producer) from = hall_ok ? base_.depot : sim::kNoTarget;
        else if (s.kind == StockKind::Supply) from = s.item.proto.speed.raw() > 0 && larva_hall ? base_.depot : (hall_ok ? base_.depot : sim::kNoTarget);
        else if (s.kind == StockKind::Army) from = larva_hall && s.item.proto.speed.raw() > 0 ? base_.depot : (prod_ok ? base_.producer : sim::kNoTarget);
        if (from == sim::kNoTarget) return;
        if (w.queue_train(from, s.item.proto, s.item.minerals, s.item.gas, s.item.build_time, s.item.count) &&
            s.kind == StockKind::Producer && base_.producer == sim::kNoTarget)
            producer_pending_ = true;
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
    run_workers(w);
    run_stock(w);
    run_production(w);
    run_expansion(w);
    run_army(w);
}

}
