#include "sc2/ai/melee_ai.hpp"

#include <utility>
#include <vector>

#include "sc2/ai/defense.hpp"
#include "sc2/ai/economy.hpp"

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
        w.queue_train(hall, wp, cfg_.worker_unit.minerals, cfg_.worker_unit.gas,
                      cfg_.worker_unit.build_time);
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
            if (cfg_.has_producer && !producer_pending_ &&
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

    if (cfg_.has_supply) {
        Fixed used = w.supply_used(player_), cap = w.supply_max(player_);
        if (cap < Fixed::from_int(200) && used + cfg_.supply_margin >= cap &&
            afford(w, cfg_.supply_unit)) {
            w.queue_train(base_.producer, cfg_.supply_unit.proto,
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
        w.queue_train(base_.producer, item.proto, item.minerals, item.gas,
                      item.build_time, item.count);
        if (in_order) ++build_idx_;
        else if (cfg_.has_army_unit2) army_alt_ = !army_alt_;
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
    if (!attacking_ && count >= cfg_.army_threshold) attacking_ = true;

    Fixed tx = cfg_.enemy_x, ty = cfg_.enemy_y;
    if (!enemy_target(w, tx, ty)) {
        tx = cfg_.enemy_x;
        ty = cfg_.enemy_y;
    }

    auto needs = [](const Unit& u, OrderType want) {
        return u.orders.empty() || u.orders.front().type != want;
    };

    if (cfg_.smart_combat && attacking_) {
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

void MeleeAI::update(sim::World& w) {
    run_workers(w);
    run_production(w);
    run_expansion(w);
    run_army(w);
}

}
