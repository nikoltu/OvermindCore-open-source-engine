#include "omc/ai/melee_ai.hpp"

#include <utility>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "omc/ai/defense.hpp"
#include "omc/ai/economy.hpp"
#include "omc/ai/scout.hpp"

namespace omc::ai {

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
           !is_worker(u.id) && !is_structure(u.id) && !is_inject_queen(u.id);
}

bool MeleeAI::is_inject_queen(uint32_t id) const {
    for (const auto& q : queen_hall_)
        if (q.first == id) return true;
    return false;
}

bool MeleeAI::evicting(const Unit& u) const {
    return respect_evict_ && !u.orders.empty() && u.orders.front().type == OrderType::Move && u.orders.front().evict;
}

bool MeleeAI::own_dropoff(const sim::World& w) const {
    for (const Unit& d : w.units())
        if (d.alive && d.player == player_ && d.drop_off_ground && !d.constructing && d.speed.raw() == 0 && !d.is_worker &&
            d.resource_left <= 0)
            return true;
    return false;
}

std::vector<uint32_t> MeleeAI::own_halls(const sim::World& w, bool spawners_only) const {
    std::vector<uint32_t> out;
    for (const Unit& d : w.units())
        if (d.alive && d.player == player_ && d.drop_off_ground && !d.constructing && d.speed.raw() == 0 && !d.is_worker &&
            !d.is_gas && d.resource_left <= 0 && (!spawners_only || d.spawner_proto >= 0))
            out.push_back(d.id);
    return out;
}

bool MeleeAI::spot_seen(const sim::World& w, Fixed x, Fixed y) const {
    for (const Unit& u : w.units()) {
        if (!u.alive || u.player != player_ || u.sight.raw() <= 0) continue;
        const Fixed r = u.sight > Fixed::from_int(2) ? u.sight - Fixed::from_int(1) : u.sight;
        const Fixed dx = u.x - x, dy = u.y - y;
        if (dx * dx + dy * dy <= r * r) return true;
    }
    return false;
}

const BuildItem* MeleeAI::stock_item(StockKind kind, bool hall_trained) const {
    for (const StockStep& s : cfg_.stock)
        if (s.kind == kind && (!hall_trained || (s.item.proto.hall_trained && s.item.proto.speed.raw() > 0))) return &s.item;
    return nullptr;
}

int MeleeAI::bases(const sim::World& w) const {
    int n = 1;
    const Fixed r = cfg_.expansion_occupied_radius;
    for (const auto& site : cfg_.expansion_sites)
        for (const Unit& d : w.units()) {
            if (!d.alive || d.player != player_ || !d.drop_off_ground || d.speed.raw() != 0 || d.is_gas) continue;
            const Fixed dx = d.x - site.first, dy = d.y - site.second;
            if (dx * dx + dy * dy <= r * r) { ++n; break; }
        }
    return n;
}

uint32_t MeleeAI::mineral_for_worker(const sim::World& w, uint32_t& hall) const {
    int best_def = -1000000;
    uint32_t best = sim::kNoTarget;
    for (uint32_t h : own_halls(w, false)) {
        const Unit& hu = w.units()[h];
        const int patches = patches_near(w, hu.x, hu.y, Fixed::from_int(10));
        if (patches <= 0) continue;
        int assigned = 0;
        for (const Unit& u : w.units())
            if (u.alive && u.player == player_ && u.is_worker && !u.gathers_gas && u.home_depot == h) ++assigned;
        const int def = 2 * patches - assigned;
        if (def > best_def) { best_def = def; best = h; }
    }
    if (best == sim::kNoTarget) return sim::kNoTarget;
    hall = best;
    return nearest_mineral(w, w.units()[best].x, w.units()[best].y);
}

bool MeleeAI::train_worker_spread(sim::World& w) {
    std::vector<std::pair<int, uint32_t>> order;
    for (uint32_t h : own_halls(w, false)) {
        if (pending_per_trainer_ && w.queue_count(h) >= 2) continue;
        const Unit& hu = w.units()[h];
        const int patches = patches_near(w, hu.x, hu.y, Fixed::from_int(10));
        if (patches <= 0) continue;
        int assigned = 0;
        for (const Unit& u : w.units())
            if (u.alive && u.player == player_ && u.is_worker && !u.gathers_gas && u.home_depot == h) ++assigned;
        order.push_back({assigned - 2 * patches, h});
    }
    std::sort(order.begin(), order.end());
    for (const auto& o : order) {
        const Unit& hu = w.units()[o.second];
        Unit wp = cfg_.worker_unit.proto;
        wp.home_depot = o.second;
        wp.gather_target = nearest_mineral(w, hu.x, hu.y);
        if (w.queue_train(o.second, wp, cfg_.worker_unit.minerals, cfg_.worker_unit.gas, cfg_.worker_unit.build_time)) {
            last_worker_train_tick_ = w.tick();
            return true;
        }
    }
    return false;
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
    if (gather_needs_dropoff_ && !own_dropoff(w)) return;
    for (uint32_t id = 0; id < U; ++id) {
        Unit& u = w.unit(id);
        if (!u.alive || u.player != player_ || !u.is_worker) continue;
        if (id == scout_) continue;
        if (w.worker_committed(id)) continue;
        if (u.orders.empty()) {
            if (gas_orphans_mine_ && u.gathers_gas && u.carrying <= 0 &&
                (u.gather_target >= U || !w.units()[u.gather_target].alive || w.units()[u.gather_target].player != player_ ||
                 w.units()[u.gather_target].resource_left <= 0))
                u.gathers_gas = false;
            if (!live_node(u.gather_target)) {
                uint32_t hall = base_.depot;
                const uint32_t m = multi_expand_ && !u.gathers_gas ? mineral_for_worker(w, hall) : sim::kNoTarget;
                if (m != sim::kNoTarget) {
                    u.home_depot = hall;
                    u.gather_target = m;
                } else {
                    u.home_depot = base_.depot;
                    u.gather_target = base_.mineral;
                }
            }
            w.issue(id, Order{OrderType::Gather, Fixed(), Fixed(), sim::kNoTarget});
        }
    }
}

void MeleeAI::run_worker_transfer(sim::World& w) {
    if (!worker_transfer_ || !multi_expand_) return;
    if (w.tick() - last_transfer_tick_ < 2 * sim::World::kTicksPerSecond) return;
    last_transfer_tick_ = w.tick();
    const std::vector<uint32_t> halls = own_halls(w, false);
    if (halls.size() < 2) return;
    const Fixed r2 = Fixed::from_int(10) * Fixed::from_int(10);
    const std::size_t H = halls.size();
    std::vector<int> ideal(H, 0), patches(H, 0), assigned(H, 0);
    for (std::size_t i = 0; i < H; ++i) {
        const Unit& hu = w.units()[halls[i]];
        for (const Unit& m : w.units()) {
            if (!m.alive || m.player >= 0 || m.resource_left <= 0 || m.is_gas) continue;
            const Fixed dx = m.x - hu.x, dy = m.y - hu.y;
            if (dx * dx + dy * dy > r2) continue;
            ++patches[i];
            ideal[i] += m.miners_max > 0 ? m.miners_max : 2;
        }
    }
    std::vector<std::vector<uint32_t>> crew(H);
    for (const Unit& u : w.units()) {
        if (!u.alive || u.player != player_ || !u.is_worker || u.gathers_gas || u.id == scout_) continue;
        if (w.worker_committed(u.id) || evicting(u)) continue;
        if (u.orders.empty() || u.orders.front().type != OrderType::Gather) continue;
        for (std::size_t i = 0; i < H; ++i)
            if (u.home_depot == halls[i]) { crew[i].push_back(u.id); break; }
    }
    for (std::size_t i = 0; i < H; ++i) assigned[i] = int(crew[i].size());
    for (int tier = 0; tier < 2; ++tier) {
        auto cap = [&](std::size_t i) { return tier == 0 ? ideal[i] : ideal[i] + patches[i]; };
        for (std::size_t s = 0; s < H; ++s) {
            std::vector<uint32_t>& c = crew[s];
            std::stable_sort(c.begin(), c.end(), [&](uint32_t a, uint32_t b) {
                return (w.units()[a].carrying > 0) < (w.units()[b].carrying > 0);
            });
            while (assigned[s] > ideal[s] && !c.empty()) {
                std::size_t best = H;
                int best_room = 0;
                for (std::size_t d = 0; d < H; ++d) {
                    if (d == s || patches[d] <= 0) continue;
                    const int room = cap(d) - assigned[d];
                    if (tier == 1 && assigned[s] - ideal[s] <= assigned[d] - ideal[d] + 1) continue;
                    if (room > best_room) { best_room = room; best = d; }
                }
                if (best == H) break;
                const uint32_t id = c.front();
                c.erase(c.begin());
                Unit& u = w.unit(id);
                const Unit& dh = w.units()[halls[best]];
                const uint32_t m = nearest_mineral(w, dh.x, dh.y);
                if (m == sim::kNoTarget) break;
                u.home_depot = halls[best];
                u.gather_target = m;
                w.issue(id, Order{OrderType::Gather, Fixed(), Fixed(), sim::kNoTarget});
                --assigned[s];
                ++assigned[best];
                ++workers_transferred_;
            }
        }
    }
}

const bool MeleeAI::kSupplyAfterProducer =std::getenv("OMC_AI_SUPPLY_AFTER_PRODUCER") != nullptr;

int MeleeAI::trainer_count(const sim::World& w) const {
    std::string pid = producer_id_;
    if (base_.producer < w.units().size() && w.units()[base_.producer].alive) pid = w.units()[base_.producer].catalog_id;
    if (pid.empty()) pid = cfg_.producer_unit.proto.catalog_id;
    int n = 0;
    for (const Unit& u : w.units()) {
        if (!u.alive || u.player != player_) continue;
        if (u.catalog_id == "Larva") { ++n; continue; }
        if (u.speed.raw() != 0 || u.constructing || u.flying) continue;
        if (u.drop_off_ground || u.id == base_.depot || (!pid.empty() && u.catalog_id == pid)) ++n;
    }
    return n;
}

int MeleeAI::pending_cap(const sim::World& w) const {
    if (!pending_per_trainer_) return cfg_.max_pending;
    return std::max(cfg_.max_pending, trainer_count(w));
}

int MeleeAI::own_pending(const sim::World& w) const {
    static const bool kWorld = std::getenv("OMC_AI_WORLD_PENDING") != nullptr;
    return int(kWorld ? w.pending_count() : w.pending_count(player_));
}

bool MeleeAI::run_supply(sim::World& w) {
    if (!cfg_.has_supply || kSupplyAfterProducer) return false;
    const Fixed used = w.supply_used(player_), cap = w.supply_max(player_);
    const std::string& sid = cfg_.supply_unit.proto.catalog_id;
    if (supply_ahead_) {
        if (cap >= Fixed::from_int(200)) return false;
        int rising = 0;
        for (const Unit& u : w.units())
            if (u.alive && u.player == player_ && u.constructing && u.supply_provides.raw() > 0) ++rising;
        for (const SiteBuild& s : site_builds_)
            if (s.id == sid) ++rising;
        if (!sid.empty()) rising += w.pending_of_type(player_, sid);
        const Fixed per = cfg_.supply_unit.proto.supply_provides.raw() > 0 ? cfg_.supply_unit.proto.supply_provides : Fixed::from_int(8);
        Fixed unit_sup = cfg_.army_unit.proto.supply_uses * Fixed::from_int(cfg_.army_unit.count > 1 ? cfg_.army_unit.count : 1);
        if (unit_sup < Fixed::from_int(1)) unit_sup = Fixed::from_int(1);
        Fixed margin = unit_sup * Fixed::from_int(2 * trainer_count(w));
        if (margin < cfg_.supply_margin) margin = cfg_.supply_margin;
        if (margin > Fixed::from_int(24)) margin = Fixed::from_int(24);
        if (used + margin < cap + per * Fixed::from_int(rising)) return false;
        if (rising >= 4) return false;
    } else {
        if (cap >= Fixed::from_int(200) || used + cfg_.supply_margin < cap) return false;
        for (const Unit& u : w.units())
            if (u.alive && u.player == player_ && u.constructing && u.supply_provides.raw() > 0) return false;
        for (const SiteBuild& s : site_builds_)
            if (s.id == sid) return false;
        if (!sid.empty() && w.pending_of_type(player_, sid) > 0) return false;
    }
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
        own_pending(w) < pending_cap(w) && afford(w, cfg_.worker_unit) &&
        supply_ok(w, cfg_.worker_unit) && multi_expand_) {
        if (cfg_.worker_train_delay_ticks <= 0 || w.tick() - last_worker_train_tick_ >= cfg_.worker_train_delay_ticks)
            train_worker_spread(w);
    } else if (cfg_.worker_target > 0 && base_.depot < w.units().size() &&
        w.unit(base_.depot).alive && worker_count(w) < cfg_.worker_target &&
        own_pending(w) < pending_cap(w) && afford(w, cfg_.worker_unit) &&
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
        const bool hall_free = !pending_per_trainer_ || w.queue_count(hall) < 2;
        if (hall_free && (cfg_.worker_train_delay_ticks <= 0 || w.tick() - last_worker_train_tick_ >= cfg_.worker_train_delay_ticks)) {
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
    if (extra_producers_ && cfg_.has_producer && cfg_.worker_builds && !stock_active() && base_.depot < w.units().size() &&
        w.unit(base_.depot).alive && w.unit(base_.depot).spawner_proto < 0) {
        const std::string& pid = w.unit(base_.producer).catalog_id;
        int halls = 0, rising = 0;
        for (const Unit& u : w.units()) {
            if (!u.alive || u.player != player_ || u.speed.raw() != 0) continue;
            if (!u.constructing && u.drop_off_ground) ++halls;
            if (u.constructing && u.catalog_id == pid) ++rising;
        }
        const int have = owned_or_pending(w, pid);
        bool walking = false;
        for (const SiteBuild& s : site_builds_)
            if (s.id == pid) walking = true;
        const int cap = multi_expand_ ? std::min(12, 1 + 3 * halls) : std::min(8, 1 + 2 * halls);
        if (!walking && rising < (multi_expand_ ? 2 : 1) && have < cap &&
            w.player(player_).minerals >= cfg_.producer_unit.minerals + 400 && afford(w, cfg_.producer_unit) &&
            cfg_.producer_unit.proto.catalog_id == pid)
            place_structure(w, cfg_.producer_unit);
    }
    const bool larva_spend = larva_macro_ && base_.depot < w.units().size() && w.unit(base_.depot).spawner_proto >= 0 &&
                             w.player(player_).minerals >= 400;
    if (own_pending(w) >= pending_cap(w) && !larva_spend) return;
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
            if (tech_counts_sites_ && !have && owned_or_pending(w, tb.proto.catalog_id) > 0) have = true;
            if (have) continue;
            if (tb.morph_self) {
                if (w.is_morphing(base_.depot)) return;
                if (!afford(w, tb)) break;
                w.player(player_).minerals -= tb.minerals;
                w.player(player_).gas -= tb.gas;
                const int32_t idx = w.add_spawn_proto(tb.proto);
                std::vector<int32_t> body_proto;
                for (const Unit& b : tb.morph_bodies) body_proto.push_back(w.add_spawn_proto(b));
                std::vector<sim::MorphStep> steps;
                for (sim::MorphStep s : tb.morph_steps) {
                    if (s.proto >= int32_t(body_proto.size())) continue;
                    s.proto = s.proto < 0 ? idx : body_proto[size_t(s.proto)];
                    steps.push_back(s);
                }
                w.begin_morph(base_.depot, idx, tb.build_time, tb.morph_finish_beh, std::move(steps), tb.morph_extras);
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

    for (int rep = 0; rep < (larva_spend ? 4 : 1); ++rep) {
    if (rep > 0 && w.player(player_).minerals < 400) break;
    const bool in_order = build_idx_ < cfg_.build_order.size();
    const bool want2 = cfg_.has_army_unit2 && army_alt_ && army_unit2_gate_ok(w);
    const BuildItem& item =
        in_order ? cfg_.build_order[build_idx_]
                 : (want2 ? cfg_.army_unit2 : cfg_.army_unit);
    auto try_item = [&](const BuildItem& it) {
        if (!afford(w, it) || !supply_ok(w, it)) return false;
        const bool morph_at_hall = base_.depot < w.units().size() &&
                                   w.unit(base_.depot).spawner_proto >= 0 &&
                                   it.proto.speed.raw() > 0;
        const uint32_t train_from = morph_at_hall ? base_.depot : base_.producer;
        return make_item(w, train_from, it);
    };
    if (try_item(item)) {
        if (in_order) ++build_idx_;
        else if (cfg_.has_army_unit2) army_alt_ = !army_alt_;
    } else if (alt_fallback_ && !in_order && cfg_.has_army_unit2) {
        const bool other2 = !want2 && army_unit2_gate_ok(w);
        if (!(want2 || other2) || !try_item(other2 ? cfg_.army_unit2 : cfg_.army_unit)) break;
    } else {
        break;
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

    if (hunt_) {
        if (w.tick() - hunt_scan_tick_ >= sim::World::kTicksPerSecond) {
            hunt_scan_tick_ = w.tick();
            run_hunt_scan(w);
        }
        int32_t foe = cfg_.match_players > 2 ? target_player(w) : -1;
        if (cfg_.match_players <= 2)
            for (const Unit& u : w.units())
                if (u.alive && u.player >= 0 && u.player != player_ && !w.are_allied(u.player, player_) &&
                    !w.player(u.player).defeated) {
                    foe = u.player;
                    break;
                }
        const Fixed mine = army_power(w, player_);
        const Fixed theirs = foe >= 0 ? army_power(w, foe) : Fixed();
        const bool committed = attacking_ || w.supply_used(player_) >= Fixed::from_int(150);
        hunting_ = committed && count > 0 && foe >= 0 && mine.raw() > 0 && theirs * Fixed::from_int(3) <= mine;        if (hunting_) {
            std::vector<uint32_t> army;
            for (const Unit& u : w.units())
                if (is_army(u) && !evicting(u)) army.push_back(u.id);
            run_hunt(w, army);
            return;
        }
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
            if (is_army(u) && !evicting(u)) army.push_back(u.id);

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
        if (!is_army(u) || evicting(u)) continue;
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
        if (!u.alive || u.player >= 0 || u.resource_left <= 0 || (multi_expand_ && u.is_gas)) continue;
        const Fixed dx = u.x - x, dy = u.y - y;
        if (dx * dx + dy * dy <= r2) ++n;
    }
    return n;
}

uint32_t MeleeAI::nearest_mineral(const sim::World& w, Fixed x, Fixed y) const {
    uint32_t best = sim::kNoTarget;
    Fixed best_d2;
    for (const Unit& u : w.units()) {
        if (!u.alive || u.player >= 0 || u.resource_left <= 0 || (multi_expand_ && u.is_gas)) continue;
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
        if (expansion_pending_ && hall_builder_ && !expand_builder_walking(w)) {
            bool rising = false;
            for (const Unit& u : w.units()) {
                if (!u.alive || u.player != player_ || u.speed.raw() != 0 || !u.drop_off_ground) continue;
                const Fixed dx = u.x - expand_x_, dy = u.y - expand_y_;
                if (dx * dx + dy * dy <= tol * tol) rising = true;
            }
            if (!rising) expansion_pending_ = false;
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
    Fixed hx = site.first, hy = site.second;
    if (hall_builder_ ? found_hall_at(w, hx, hy)
                      : w.queue_train_at(base_.depot, cfg_.expansion_unit.proto, cfg_.expansion_unit.minerals,
                                         cfg_.expansion_unit.gas, cfg_.expansion_unit.build_time, site.first,
                                         site.second)) {
        expansion_pending_ = true;
        expand_x_ = hx;
        expand_y_ = hy;
    }
}

bool MeleeAI::expand_builder_walking(const sim::World& w) const {
    return expand_builder_ < w.units().size() && w.units()[expand_builder_].alive &&
           w.units()[expand_builder_].player == player_ && w.worker_committed(expand_builder_) &&
           w.units()[expand_builder_].construct_target == sim::kNoTarget;
}

bool MeleeAI::found_hall_at(sim::World& w, Fixed& sx, Fixed& sy) {
    const std::size_t U = w.units().size();
    Unit proto = cfg_.expansion_unit.proto;
    proto.player = player_;
    HallSpot* spot = nullptr;
    for (HallSpot& h : hall_spots_)
        if (h.site_x == sx && h.site_y == sy) spot = &h;
    if (!spot) {
        hall_spots_.push_back({sx, sy, sx, sy});
        spot = &hall_spots_.back();
        if (!w.resource_halo_ok(proto, sx, sy)) {
            const sim::NavGrid& nav = w.nav();
            bool found = false;
            Fixed best_d2;
            for (int dy = -5; dy <= 5; ++dy)
                for (int dx = -5; dx <= 5; ++dx) {
                    if (dx * dx + dy * dy > 25) continue;
                    const Fixed x = sx + Fixed::from_int(dx), y = sy + Fixed::from_int(dy);
                    if (!w.resource_halo_ok(proto, x, y)) continue;
                    bool open = true;
                    for (int oy = -2; oy <= 2 && open && nav.configured(); ++oy)
                        for (int ox = -2; ox <= 2; ++ox)
                            if (nav.is_blocked(x + Fixed::from_int(ox), y + Fixed::from_int(oy))) { open = false; break; }
                    if (!open) continue;
                    const Fixed d2 = Fixed::from_int(dx * dx + dy * dy);
                    if (found && d2 >= best_d2) continue;
                    found = true;
                    best_d2 = d2;
                    spot->x = x;
                    spot->y = y;
                }
        }
    }
    sx = spot->x;
    sy = spot->y;
    uint32_t builder = sim::kNoTarget;
    Fixed best_d2;
    for (uint32_t id = 0; id < U; ++id) {
        const Unit& u = w.unit(id);
        if (!u.alive || u.player != player_ || !u.is_worker || u.loaded || u.in_gas_structure || id == scout_) continue;
        if (w.worker_committed(id) || u.gathers_gas) continue;
        const Fixed dx = u.x - sx, dy = u.y - sy, d2 = dx * dx + dy * dy;
        if (builder == sim::kNoTarget || d2 < best_d2) { builder = id; best_d2 = d2; }
    }
    if (builder == sim::kNoTarget) return false;
    if (!w.issue_worker_build(builder, sx, sy, proto, cfg_.expansion_unit.minerals, cfg_.expansion_unit.gas,
                              cfg_.expansion_unit.build_time, Fixed::from_int(2), w.unit(builder).build_verb)) {
        if (std::getenv("OMC_AI_TRACE"))
            std::fprintf(stderr, "[ai] p%d tick %d %s at site (%.1f,%.1f): refused (%s)\n", int(player_), int(w.tick()),
                         proto.catalog_id.c_str(), sx.to_double(), sy.to_double(), sim::cmd_error_key(w.last_error()));
        return false;
    }
    site_builds_.push_back({proto.catalog_id, builder});
    expand_builder_ = builder;
    return true;
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
    const std::size_t U = w.units().size();
    const bool spread = pending_per_trainer_ && cfg_.worker_builds && item.proto.speed.raw() > 0 && from != base_.depot &&
                        from < U && w.queue_count(from) > 0;
    if (!spread && w.queue_train(from, item.proto, item.minerals, item.gas, item.build_time, item.count)) return true;
    if (larva_macro_ && from < U && w.unit(from).spawner_proto >= 0 && item.proto.speed.raw() > 0 && !item.proto.hall_trained) {
        for (uint32_t h : own_halls(w, true))
            if (h != from && w.queue_train(h, item.proto, item.minerals, item.gas, item.build_time, item.count)) return true;
        return false;
    }
    if (!cfg_.worker_builds || item.proto.speed.raw() == 0 || from == base_.depot) return false;
    std::vector<std::pair<int32_t, uint32_t>> others;
    if (spread) others.push_back({w.queue_count(from), from});
    for (uint32_t id = 0; id < U; ++id) {
        const Unit& u = w.unit(id);
        if (!u.alive || u.player != player_ || u.speed.raw() != 0 || u.constructing || u.flying) continue;
        if (id == from || id == base_.depot || u.is_gas || u.addon_body_kind != 0) continue;
        if (pending_per_trainer_ && from < U && u.catalog_id != w.unit(from).catalog_id) continue;
        others.push_back({w.queue_count(id), id});
    }
    std::sort(others.begin(), others.end());
    for (const std::pair<int32_t, uint32_t>& o : others) {
        if (pending_per_trainer_ && o.first >= 2) break;
        if (w.queue_train(o.second, item.proto, item.minerals, item.gas, item.build_time, item.count)) return true;
    }
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
        std::vector<uint32_t> anchors{base_.depot};
        if (multi_expand_)
            for (uint32_t h : own_halls(w, false))
                if (h != base_.depot) anchors.push_back(h);
        for (std::size_t a = 0; a < anchors.size() && node == sim::kNoTarget; ++a) {
            const Fixed ax = w.unit(anchors[a]).x, ay = w.unit(anchors[a]).y;
            for (uint32_t id = 0; id < U; ++id) {
                const Unit& g = w.unit(id);
                if (!g.alive || !g.is_gas || g.resource_left <= 0 || g.player >= 0) continue;
                const Fixed dx = g.x - ax, dy = g.y - ay;
                const Fixed d2 = dx * dx + dy * dy;
                if (d2 > Fixed::from_int(14 * 14)) continue;
                if (node == sim::kNoTarget || d2 < node_d2) { node = id; node_d2 = d2; }
            }
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
        !std::getenv("OMC_AI_NO_POWER_FIRST")) {
        bool own_field = false;
        for (const Unit& o : w.units())
            if (o.alive && o.player == player_ && o.power_radius.raw() > 0) { own_field = true; break; }
        if (!own_field) {
            const bool placed = afford(w, cfg_.supply_unit) && place_structure(w, cfg_.supply_unit);
            if (std::getenv("OMC_AI_TRACE"))
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
    static const bool kAnyLevel = std::getenv("OMC_AI_ANY_LEVEL_SITE") != nullptr;
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
        cfg_.supply_unit.proto.power_radius.raw() > 0 && !std::getenv("OMC_AI_NO_POWER_FIRST")) {
        bool rising = false;
        for (const Unit& o : w.units())
            if (o.alive && o.player == player_ && o.power_radius.raw() > 0 && o.constructing) { rising = true; break; }
        if (!rising && afford(w, cfg_.supply_unit)) place_structure(w, cfg_.supply_unit);
    }
    if (std::getenv("OMC_AI_TRACE"))
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
            if (id == scout_ || w.worker_committed(id) || evicting(u)) continue;
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
        if (s.kind == StockKind::Skip || (s.kind == StockKind::Gas && !stock_gas_)) { ++stock_idx_; continue; }
        if (s.kind == StockKind::Gas) {
            if (owned_or_pending(w, s.id) >= s.count || !free_geyser_near_hall(w)) { ++stock_idx_; continue; }
            if (own_pending(w) >= pending_cap(w) || !afford(w, s.item)) return;
            if (base_.depot < U && w.unit(base_.depot).alive) make_item(w, base_.depot, s.item);
            return;
        }
        if (stock_gas_ && s.item.gas > 0 && w.player(player_).gas < s.item.gas && !has_gas_source(w)) { ++stock_idx_; continue; }
        if (s.kind == StockKind::Worker) {
            if (cfg_.worker_target < s.count) cfg_.worker_target = s.count;
            if (worker_count(w) + w.pending_of_type(player_, s.id) >= s.count) { ++stock_idx_; continue; }
            return;
        }
        if (owned_or_pending(w, s.id) >= s.count) { ++stock_idx_; continue; }
        if (own_pending(w) >= pending_cap(w)) return;
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

bool MeleeAI::has_gas_source(const sim::World& w) const {
    for (const Unit& u : w.units())
        if (u.alive && u.player == player_ && u.is_gas && u.speed.raw() == 0) return true;
    for (const SiteBuild& sb : site_builds_)
        if (sb.id == "Refinery" || sb.id == "Assimilator" || sb.id == "Extractor") return true;
    return false;
}

bool MeleeAI::free_geyser_near_hall(const sim::World& w) const {
    if (base_.depot >= w.units().size() || !w.units()[base_.depot].alive) return false;
    const Unit& h = w.units()[base_.depot];
    for (const Unit& g : w.units()) {
        if (!g.alive || !g.is_gas || g.resource_left <= 0 || g.player >= 0) continue;
        const Fixed dx = g.x - h.x, dy = g.y - h.y;
        if (dx * dx + dy * dy <= Fixed::from_int(14 * 14)) return true;
    }
    return false;
}

void MeleeAI::run_multi_expand(sim::World& w) {
    if (!cfg_.has_expansion) return;
    const std::size_t U = w.units().size();
    const std::string& hid = cfg_.expansion_unit.proto.catalog_id;
    if (expansion_pending_) {
        const Fixed tol = Fixed::from_int(3);
        uint32_t found = sim::kNoTarget;
        for (const Unit& u : w.units()) {
            if (!u.alive || u.player != player_ || u.speed.raw() != 0 || !u.drop_off_ground) continue;
            const Fixed dx = u.x - expand_x_, dy = u.y - expand_y_;
            if (dx * dx + dy * dy <= tol * tol) { found = u.id; break; }
        }
        if (found != sim::kNoTarget) {
            expansion_pending_ = false;
            const int patches = patches_near(w, expand_x_, expand_y_, Fixed::from_int(10));
            cfg_.worker_target = std::min(70, cfg_.worker_target + 2 * patches + 2 * cfg_.gas_peons_per_geyser);
            if (base_.expansion == sim::kNoTarget) {
                base_.expansion = found;
                base_.expansion_mineral = nearest_mineral(w, expand_x_, expand_y_);
            }
        } else if (hall_builder_ ? !expand_builder_walking(w) : w.pending_of_type(player_, hid) == 0) {
            expansion_pending_ = false;
        }
        if (expansion_pending_) return;
    }
    if (base_.depot >= U || !w.unit(base_.depot).alive) return;
    const int nb = bases(w);
    const int bank = w.player(player_).minerals, cost = cfg_.expansion_unit.minerals;
    const BuildItem* gas = stock_item(StockKind::Gas, false);
    const int used = int(w.supply_used(player_).to_double());
    if (gas && stock_gas_ && !stock_active() && nb >= 2 && used >= 30 && bank >= gas->minerals + 150 &&
        w.player(player_).gas < 300) {
        int gas_have = 0, mining_halls = 0;
        bool walking = false;
        for (const Unit& u : w.units())
            if (u.alive && u.player == player_ && u.is_gas && u.speed.raw() == 0) ++gas_have;
        for (const SiteBuild& s : site_builds_)
            if (s.id == gas->proto.catalog_id) walking = true;
        for (uint32_t h : own_halls(w, false))
            if (patches_near(w, w.unit(h).x, w.unit(h).y, Fixed::from_int(10)) > 0) ++mining_halls;
        if (!walking && gas_have < 2 * mining_halls) place_structure(w, *gas);
    }
    if (nb >= 4 || bank < cost) return;
    int ideal = 0;
    for (uint32_t h : own_halls(w, false)) ideal += 2 * patches_near(w, w.unit(h).x, w.unit(h).y, Fixed::from_int(10));
    const bool saturated = ideal <= 0 || worker_count(w) * 10 >= ideal * 8;
    const bool floating = bank >= cost + 400;
    const int gate = nb == 1 ? 0 : nb == 2 ? 40 : 80;
    if (!saturated && !floating) return;
    if (used < gate && bank < cost + 800) return;
    std::vector<std::pair<Fixed, Fixed>> taken;
    for (const Unit& u : w.units())
        if (u.alive && u.speed.raw() == 0 && u.supply_provides.raw() > 0) taken.push_back({u.x, u.y});
    const Unit& depot = w.unit(base_.depot);
    const int idx = nearest_expansion(cfg_.expansion_sites, taken, depot.x, depot.y, cfg_.expansion_occupied_radius);
    if (idx < 0) return;
    const auto& site = cfg_.expansion_sites[std::size_t(idx)];
    if (hall_builder_) {
        Fixed hx = site.first, hy = site.second;
        if (found_hall_at(w, hx, hy)) {
            expansion_pending_ = true;
            expand_x_ = hx;
            expand_y_ = hy;
        }
        return;
    }
    std::vector<uint32_t> from{base_.depot};
    for (uint32_t h : own_halls(w, false))
        if (h != base_.depot) from.push_back(h);
    for (uint32_t f : from)
        if (w.queue_train_at(f, cfg_.expansion_unit.proto, cfg_.expansion_unit.minerals, cfg_.expansion_unit.gas,
                             cfg_.expansion_unit.build_time, site.first, site.second)) {
            expansion_pending_ = true;
            expand_x_ = site.first;
            expand_y_ = site.second;
            return;
        }
}

void MeleeAI::run_larva_macro(sim::World& w) {
    const std::size_t U = w.units().size();
    if (base_.depot >= U || w.unit(base_.depot).spawner_proto < 0) return;
    const std::vector<uint32_t> halls = own_halls(w, true);
    for (std::size_t i = 0; i < queen_hall_.size();) {
        const uint32_t q = queen_hall_[i].first, h = queen_hall_[i].second;
        const bool ok = q < U && w.unit(q).alive && w.unit(q).player == player_ && h < U && w.unit(h).alive &&
                        w.unit(h).player == player_;
        if (ok) ++i;
        else queen_hall_.erase(queen_hall_.begin() + long(i));
    }
    const BuildItem* queen = stock_item(StockKind::Army, true);
    if (queen) {
        const std::string& qid = queen->proto.catalog_id;
        int owned = 0;
        for (const Unit& u : w.units()) {
            if (!u.alive || u.player != player_ || u.catalog_id != qid) continue;
            ++owned;
            if (is_inject_queen(u.id)) continue;
            uint32_t best = sim::kNoTarget;
            Fixed bd;
            for (uint32_t h : halls) {
                bool has = false;
                for (const auto& qh : queen_hall_)
                    if (qh.second == h) has = true;
                if (has) continue;
                const Fixed dx = w.unit(h).x - u.x, dy = w.unit(h).y - u.y, d2 = dx * dx + dy * dy;
                if (best == sim::kNoTarget || d2 < bd) { best = h; bd = d2; }
            }
            if (best != sim::kNoTarget) queen_hall_.push_back({u.id, best});
        }
        for (const auto& qh : queen_hall_) {
            Unit& qu = w.unit(qh.first);
            if (!qu.orders.empty() || evicting(qu)) continue;
            const Unit& h = w.unit(qh.second);
            const Fixed dx = h.x - qu.x, dy = h.y - qu.y;
            if (dx * dx + dy * dy > Fixed::from_int(7 * 7)) {
                const Fixed d = sim::isqrt(dx * dx + dy * dy);
                const Fixed stand = h.radius + Fixed::from_int(2);
                w.issue(qh.first, Order{OrderType::Move, h.x - dx * stand / d, h.y - dy * stand / d, sim::kNoTarget});
            }
        }
        const int want = std::min(6, int(halls.size()));
        if (!stock_active() && owned + w.pending_of_type(player_, qid) < want && afford(w, *queen) && supply_ok(w, *queen))
            for (uint32_t h : halls) {
                bool has = false;
                for (const auto& qh : queen_hall_)
                    if (qh.second == h) has = true;
                if (has || w.queue_count(h) > 0) continue;
                if (w.queue_train(h, queen->proto, queen->minerals, queen->gas, queen->build_time)) break;
            }
    }
    if (!cfg_.has_expansion || stock_active() || expansion_pending_) return;
    const int cost = cfg_.expansion_unit.minerals;
    if (w.player(player_).minerals < cost + 300) return;
    int larva = 0;
    for (const Unit& u : w.units())
        if (u.alive && u.player == player_ && u.catalog_id == "Larva") ++larva;
    if (larva > int(halls.size())) return;
    const std::string& hid = cfg_.expansion_unit.proto.catalog_id;
    for (const Unit& u : w.units())
        if (u.alive && u.player == player_ && u.constructing && u.catalog_id == hid) return;
    for (const SiteBuild& s : site_builds_)
        if (s.id == hid) return;
    if (int(halls.size()) >= bases(w) + 1) return;
    place_structure(w, cfg_.expansion_unit);
}

void MeleeAI::run_hunt_scan(sim::World& w) {
    const std::size_t U = w.units().size();
    for (const Unit& e : w.units()) {
        if (!e.alive || e.player < 0 || e.player == player_ || w.are_allied(e.player, player_) || w.player(e.player).defeated) continue;
        if (w.defeat_rule_authored() ? !e.prevent_defeat : e.speed.raw() != 0) continue;
        if (!w.visible_to(player_, e)) continue;
        bool known = false;
        for (Seen& s : seen_)
            if (s.id == e.id) { s.x = e.x; s.y = e.y; known = true; break; }
        if (!known) seen_.push_back(Seen{e.id, e.x, e.y});
    }
    for (std::size_t i = 0; i < seen_.size();) {
        const Seen& s = seen_[i];
        const Unit* e = s.id < U ? &w.units()[s.id] : nullptr;
        const bool there = e && e->alive && e->player >= 0 && e->player != player_ && !w.player(e->player).defeated &&
                           e->x == s.x && e->y == s.y;
        if (!there && spot_seen(w, s.x, s.y)) seen_.erase(seen_.begin() + long(i));
        else ++i;
    }
    if (hunting_)
        for (HuntPoint& p : hunt_points_) {
            if (!p.done && spot_seen(w, p.x, p.y)) {
                p.done = true;
                p.done_tick = w.tick();
            } else if (p.done && p.site && w.tick() - p.done_tick > 90 * sim::World::kTicksPerSecond) {
                p.done = false;
                p.since = -1;
            }
        }
}

void MeleeAI::run_hunt(sim::World& w, const std::vector<uint32_t>& army) {
    if (army.empty()) return;
    if (hunt_points_.empty()) {
        hunt_points_.push_back(HuntPoint{cfg_.enemy_x, cfg_.enemy_y, false, -1, true, 0});
        std::vector<std::pair<Fixed, Fixed>> sites = cfg_.expansion_sites;
        std::sort(sites.begin(), sites.end(), [&](const auto& a, const auto& b) {
            const std::int64_t ax = (a.first - cfg_.enemy_x).raw(), ay = (a.second - cfg_.enemy_y).raw();
            const std::int64_t bx = (b.first - cfg_.enemy_x).raw(), by = (b.second - cfg_.enemy_y).raw();
            return ax * ax + ay * ay < bx * bx + by * by;
        });
        for (const auto& s : sites) hunt_points_.push_back(HuntPoint{s.first, s.second, false, -1, true, 0});
        if (hunt_gas_)
            for (const Unit& g : w.units()) {
                if (!g.alive || !g.is_gas || (g.player >= 0 && (g.player == player_ || w.are_allied(g.player, player_)))) continue;
                bool dup = false;
                for (const HuntPoint& p : hunt_points_) {
                    const Fixed dx = p.x - g.x, dy = p.y - g.y;
                    if (dx * dx + dy * dy <= Fixed::from_int(1)) { dup = true; break; }
                }
                if (!dup) hunt_points_.push_back(HuntPoint{g.x, g.y, false, -1, true, 0});
            }
        if (has_bounds_) {
            const int x0 = bounds_.min_x.floor() + 8, x1 = bounds_.max_x.floor() - 8;
            const int y0 = bounds_.min_y.floor() + 8, y1 = bounds_.max_y.floor() - 8;
            for (int y = y0; y <= y1; y += 16)
                for (int x = x0; x <= x1; x += 16) {
                    const Fixed fx = Fixed::from_int(x), fy = Fixed::from_int(y);
                    if (w.nav().configured() && w.nav().is_blocked(fx, fy)) continue;
                    hunt_points_.push_back(HuntPoint{fx, fy});
                }
        }
    }
    auto reissue = [&](uint32_t id, const Order& o) {
        const Unit& u = w.unit(id);
        if (!u.orders.empty() && u.orders.front().type == o.type) {
            const Order& f = u.orders.front();
            if (o.type == OrderType::Attack && f.target == o.target) return;
            if (o.type == OrderType::AttackMove) {
                const Fixed dx = f.tx - o.tx, dy = f.ty - o.ty;
                if (dx * dx + dy * dy <= Fixed::from_int(1)) return;
            }
        }
        w.issue(id, o);
    };
    if (!seen_.empty()) {
        Fixed cx, cy;
        for (uint32_t id : army) { cx = cx + w.unit(id).x; cy = cy + w.unit(id).y; }
        cx = cx / Fixed::from_int(int(army.size()));
        cy = cy / Fixed::from_int(int(army.size()));
        std::size_t best = 0;
        std::int64_t bd = -1;
        for (std::size_t i = 0; i < seen_.size(); ++i) {
            const std::int64_t dx = (seen_[i].x - cx).raw(), dy = (seen_[i].y - cy).raw();
            const std::int64_t d2 = dx * dx + dy * dy;
            if (bd < 0 || d2 < bd) { bd = d2; best = i; }
        }
        const Seen& t = seen_[best];
        const bool live = t.id < w.units().size() && w.unit(t.id).alive && w.visible_to(player_, w.unit(t.id));
        for (uint32_t id : army) {
            const Unit& u = w.unit(id);
            const Fixed dx = t.x - u.x, dy = t.y - u.y;
            if (live && dx * dx + dy * dy <= Fixed::from_int(12 * 12))
                reissue(id, Order{OrderType::Attack, Fixed(), Fixed(), t.id});
            else
                reissue(id, Order{OrderType::AttackMove, t.x, t.y, sim::kNoTarget});
        }
        return;
    }
    bool any_open = false;
    for (const HuntPoint& p : hunt_points_)
        if (!p.done) any_open = true;
    if (!any_open)
        for (HuntPoint& p : hunt_points_) {
            p.done = false;
            p.since = -1;
        }
    const std::size_t squads = std::min<std::size_t>(4, std::max<std::size_t>(1, army.size() / 3));
    std::vector<std::size_t> taken;
    for (std::size_t k = 0; k < squads; ++k) {
        Fixed cx, cy;
        int n = 0;
        for (std::size_t i = k; i < army.size(); i += squads) {
            cx = cx + w.unit(army[i]).x;
            cy = cy + w.unit(army[i]).y;
            ++n;
        }
        if (n == 0) continue;
        cx = cx / Fixed::from_int(n);
        cy = cy / Fixed::from_int(n);
        std::size_t best = hunt_points_.size();
        std::int64_t bd = -1;
        for (int pass = 0; pass < 2 && best == hunt_points_.size(); ++pass)
            for (std::size_t i = 0; i < hunt_points_.size(); ++i) {
                if (hunt_points_[i].done) continue;
                if (pass == 0 && std::find(taken.begin(), taken.end(), i) != taken.end()) continue;
                const std::int64_t dx = (hunt_points_[i].x - cx).raw() >> 4, dy = (hunt_points_[i].y - cy).raw() >> 4;
                const std::int64_t d2 = (dx * dx + dy * dy) * (hunt_points_[i].site ? 1 : 4);
                if (bd < 0 || d2 < bd) { bd = d2; best = i; }
            }
        if (best == hunt_points_.size()) continue;
        taken.push_back(best);
        HuntPoint& p = hunt_points_[best];
        if (p.since < 0) p.since = w.tick();
        else if (w.tick() - p.since > 60 * sim::World::kTicksPerSecond) {
            p.done = true;
            p.done_tick = w.tick();
        }
        for (std::size_t i = k; i < army.size(); i += squads)
            reissue(army[i], Order{OrderType::AttackMove, p.x, p.y, sim::kNoTarget});
    }
}

void MeleeAI::update(sim::World& w) {
    if (multi_expand_ && (base_.depot >= w.units().size() || !w.unit(base_.depot).alive)) {
        const std::vector<uint32_t> halls = own_halls(w, false);
        if (!halls.empty()) {
            base_.depot = halls.front();
            base_.mineral = nearest_mineral(w, w.unit(base_.depot).x, w.unit(base_.depot).y);
        }
    }
    run_scout(w);
    prune_site_builds(w);
    run_workers(w);
    run_worker_transfer(w);
    run_gas(w);
    run_stock(w);
    run_production(w);
    if (multi_expand_) run_multi_expand(w);
    else run_expansion(w);
    if (larva_macro_) run_larva_macro(w);
    run_army(w);
}

}
