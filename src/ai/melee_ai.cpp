#include "sc2/ai/melee_ai.hpp"

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
    return u.alive && u.player == player_ && u.speed.raw() > 0 &&
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
    return p.minerals >= it.minerals && p.gas >= it.gas;
}

bool MeleeAI::supply_ok(const sim::World& w, const BuildItem& it) const {
    return (w.supply_used(player_) + it.proto.supply_uses) <= w.supply_max(player_);
}

void MeleeAI::run_workers(sim::World& w) {
    for (uint32_t wid : base_.workers) {
        if (wid >= w.units().size()) continue;
        Unit& u = w.unit(wid);
        if (u.alive && u.orders.empty()) {
            u.home_depot = base_.depot;
            u.gather_target = base_.mineral;
            w.issue(wid, Order{OrderType::Gather, Fixed(), Fixed(), sim::kNoTarget});
        }
    }
}

void MeleeAI::run_production(sim::World& w) {
    if (base_.producer >= w.units().size() || !w.unit(base_.producer).alive) return;
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
    const BuildItem& item = in_order ? cfg_.build_order[build_idx_] : cfg_.army_unit;
    if (afford(w, item) && supply_ok(w, item)) {
        w.queue_train(base_.producer, item.proto, item.minerals, item.gas,
                      item.build_time);
        if (in_order) ++build_idx_;
    }
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

void MeleeAI::update(sim::World& w) {
    run_workers(w);
    run_production(w);
    run_army(w);
}

}
