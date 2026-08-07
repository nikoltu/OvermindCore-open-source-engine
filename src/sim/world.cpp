#include "sc2/sim/world.hpp"

#include <algorithm>

#include "sc2/sim/combat.hpp"

namespace sc2::sim {

Fixed World::dist2(const Unit& a, const Unit& b) {
    Fixed dx = a.x - b.x, dy = a.y - b.y;
    return dx * dx + dy * dy;
}

int World::alive_count(int player) const {
    int n = 0;
    for (const Unit& u : units_)
        if (u.alive && u.player == player) ++n;
    return n;
}

int World::winner() const {
    int found = -1;
    for (const Unit& u : units_) {
        if (!u.alive || u.player < 0) continue;
        if (found == -1)
            found = u.player;
        else if (found != u.player)
            return -1;
    }
    return found;
}

bool World::visible_to(int32_t player, const Unit& target) const {
    for (const Unit& a : units_) {
        if (!a.alive || a.sight.raw() <= 0) continue;
        if (!are_allied(a.player, player)) continue;
        const Fixed dx = target.x - a.x, dy = target.y - a.y;
        if (dx * dx + dy * dy <= a.sight * a.sight) return true;
    }
    return false;
}

uint32_t World::acquire_target(const Unit& u) const {
    uint32_t best = kNoTarget;
    Fixed best_d2;
    for (const Unit& o : units_) {
        if (!o.alive || are_allied(o.player, u.player)) continue;
        if (o.loaded) continue;
        if ((o.burrowed || o.cloaked_effective()) && !detected_by(u.player, o)) continue;
        {
            const uint32_t plane_bit = o.flying ? 2u : 1u;
            const bool w1 = (u.wpn_target_filter & plane_bit) != 0;
            const bool w2 = u.wpn2_damage.raw() > 0 && (u.wpn2_target_filter & plane_bit) != 0;
            if (!w1 && !w2) continue;
        }
        if (fog_acquire_ && !visible_to(u.player, o)) continue;
        Fixed d2 = dist2(u, o);
        if (best == kNoTarget || d2 < best_d2) {
            best = o.id;
            best_d2 = d2;
        }
    }
    return best;
}

namespace {
bool move_toward(Unit& u, Fixed tx, Fixed ty, Fixed step) {
    Fixed dx = tx - u.x, dy = ty - u.y;
    Fixed d2 = dx * dx + dy * dy;
    if (d2 <= step * step) {
        u.x = tx;
        u.y = ty;
        return true;
    }
    Fixed dist = isqrt(d2);
    if (dist.raw() > 0) {
        u.x = u.x + dx * step / dist;
        u.y = u.y + dy * step / dist;
    }
    return false;
}
}

Fixed World::move_step(const Unit& u, Fixed dt) const {
    Fixed speed = u.speed;
    if (creep_.is_creep(u.x, u.y)) speed = speed * u.speed_mult_creep;
    if (!u.behaviors.empty()) speed = speed * behavior_speed_mult(u);
    return speed * dt;
}

bool World::move_unit_toward(Unit& u, Fixed gx, Fixed gy, Fixed step) {
    if (!nav_.has_blocked() || u.flying) {
        return move_toward(u, gx, gy, step);
    }

    int scx, scy, gcx, gcy;
    nav_.to_cell(u.x, u.y, scx, scy);
    nav_.to_cell(gx, gy, gcx, gcy);

    const bool need_plan = u.path.empty() || u.path_next >= u.path.size() ||
                           u.path_gx != gcx || u.path_gy != gcy;
    if (need_plan) {
        std::vector<NavCell> cells;
        find_path(nav_, scx, scy, gcx, gcy, cells);
        u.path.clear();
        u.path_next = 0;
        u.path_gx = gcx;
        u.path_gy = gcy;
        for (size_t k = 1; k < cells.size(); ++k) {
            Fixed wx, wy;
            nav_.cell_center(cells[k].x, cells[k].y, wx, wy);
            u.path.push_back(Waypoint{wx, wy});
        }
        if (!u.path.empty())
            u.path.back() = Waypoint{gx, gy};
        else
            u.path.push_back(Waypoint{gx, gy});
    }

    if (u.path_next < u.path.size()) {
        const Waypoint wp = u.path[u.path_next];
        if (move_toward(u, wp.x, wp.y, step)) {
            ++u.path_next;
            if (u.path_next >= u.path.size()) {
                u.path.clear();
                u.path_next = 0;
                return true;
            }
        }
        return false;
    }
    return true;
}

void World::apply_separation() {
    if (!separation_) return;
    const size_t n = units_.size();
    std::vector<Fixed> ax(n), ay(n);
    for (size_t i = 0; i < n; ++i) {
        if (!units_[i].alive) continue;
        for (size_t j = i + 1; j < n; ++j) {
            if (!units_[j].alive) continue;
            const Unit& a = units_[i];
            const Unit& b = units_[j];
            const Fixed rsum = a.radius + b.radius;
            if (rsum.raw() <= 0) continue;
            const Fixed dx = b.x - a.x, dy = b.y - a.y;
            const Fixed d2 = dx * dx + dy * dy;
            if (d2 >= rsum * rsum) continue;
            Fixed nx, ny, pen;
            if (d2.raw() > 0) {
                const Fixed d = isqrt(d2);
                if (d.raw() <= 0) continue;
                pen = rsum - d;
                nx = dx / d;
                ny = dy / d;
            } else {
                pen = rsum;
                nx = Fixed::from_int(1);
                ny = Fixed();
            }
            const bool mov_i = a.speed.raw() != 0;
            const bool mov_j = b.speed.raw() != 0;
            if (!mov_i && !mov_j) continue;
            Fixed pi, pj;
            if (mov_i && mov_j) {
                pi = pen / Fixed::from_int(4);
                pj = pi;
            } else if (mov_i) {
                pi = pen / Fixed::from_int(2);
                pj = Fixed();
            } else {
                pi = Fixed();
                pj = pen / Fixed::from_int(2);
            }
            ax[i] = ax[i] - nx * pi;
            ay[i] = ay[i] - ny * pi;
            ax[j] = ax[j] + nx * pj;
            ay[j] = ay[j] + ny * pj;
        }
    }
    for (size_t i = 0; i < n; ++i) {
        if (!units_[i].alive) continue;
        units_[i].x = units_[i].x + ax[i];
        units_[i].y = units_[i].y + ay[i];
    }
}

Player& World::player(int32_t p) {
    if (p < 0) p = 0;
    while (int32_t(players_.size()) <= p) players_.push_back(Player{});
    return players_[p];
}

const Player& World::player(int32_t p) const {
    static const Player kDefault{};
    if (p < 0 || p >= int32_t(players_.size())) return kDefault;
    return players_[p];
}

int32_t World::team_of(int32_t p) const {
    if (p < 0 || p >= int32_t(players_.size())) return -1;
    return players_[p].team;
}

bool World::are_allied(int32_t a, int32_t b) const {
    if (a == b) return true;
    const int32_t ta = team_of(a);
    return ta >= 0 && ta == team_of(b);
}

int32_t World::upgrade_level(int32_t player, int32_t upgrade_id) const {
    if (player < 0 || player >= int32_t(players_.size()) || upgrade_id < 0) return 0;
    const std::vector<int32_t>& up = players_[player].upgrades;
    return upgrade_id < int32_t(up.size()) ? up[upgrade_id] : 0;
}

void World::set_upgrade_level(int32_t player_id, int32_t upgrade_id, int32_t level) {
    if (upgrade_id < 0) return;
    Player& pl = player(player_id);
    if (int32_t(pl.upgrades.size()) <= upgrade_id) pl.upgrades.resize(upgrade_id + 1, 0);
    pl.upgrades[upgrade_id] = level;
}

Order World::smart_order(uint32_t actor, uint32_t target, Fixed tx, Fixed ty) const {
    if (target < units_.size() && units_[target].alive) {
        const Unit& t = units_[target];
        if (t.resource_left > 0) {
            Order o;
            o.type = OrderType::Gather;
            o.target = target;
            return o;
        }
        const int32_t ap = actor < units_.size() ? units_[actor].player : 0;
        if (!are_allied(t.player, ap)) {
            Order o;
            o.type = OrderType::Attack;
            o.target = target;
            return o;
        }
        Order o;
        o.type = OrderType::Move;
        o.tx = t.x;
        o.ty = t.y;
        return o;
    }
    Order o;
    o.type = OrderType::Move;
    o.tx = tx;
    o.ty = ty;
    return o;
}

Fixed World::supply_used(int32_t p) const {
    Fixed s;
    for (const Unit& u : units_)
        if (u.alive && u.player == p) s = s + u.supply_uses;
    return s;
}

Fixed World::supply_max(int32_t p) const {
    Fixed s;
    for (const Unit& u : units_)
        if (u.alive && u.player == p && !u.constructing) s = s + u.supply_provides;
    Fixed cap = Fixed::from_int(200);
    return s > cap ? cap : s;
}

int32_t World::add_veterancy(VeterancyDef def) {
    vet_defs_.push_back(std::move(def));
    return int32_t(vet_defs_.size()) - 1;
}

int32_t World::add_ability(AbilityDef def) {
    abilities_.push_back(def);
    return int32_t(abilities_.size()) - 1;
}

void World::add_behavior(uint32_t id, const BehaviorInstance& b) {
    if (id >= units_.size()) return;
    Unit& u = units_[id];
    if (b.kind != 0) {
        for (BehaviorInstance& e : u.behaviors) {
            if (e.kind == b.kind) {
                e = b;
                return;
            }
        }
    }
    u.behaviors.push_back(b);
}

Fixed World::behavior_speed_mult(const Unit& u) {
    Fixed m = Fixed::from_int(1);
    for (const BehaviorInstance& b : u.behaviors) m = m * b.speed_mult;
    return m;
}
bool World::behavior_stunned(const Unit& u) {
    for (const BehaviorInstance& b : u.behaviors)
        if (b.stun) return true;
    return false;
}
bool World::behavior_rooted(const Unit& u) {
    for (const BehaviorInstance& b : u.behaviors)
        if (b.root || b.stun) return true;
    return false;
}

int32_t World::add_persistent(PersistentInstance p) {
    persistents_.push_back(p);
    return int32_t(persistents_.size()) - 1;
}

void World::step_cast(Unit& u, Fixed dt) {
    Order& o = u.orders.front();
    if (o.ability >= abilities_.size()) {
        u.orders.erase(u.orders.begin());
        return;
    }
    const AbilityDef ab = abilities_[o.ability];

    if (ab.kind == EffectKind::Teleport) {
        const Fixed dx = o.tx - u.x, dy = o.ty - u.y;
        if (ab.range.raw() <= 0 || dx * dx + dy * dy <= ab.range * ab.range) {
            u.x = o.tx;
            u.y = o.ty;
        }
        u.orders.erase(u.orders.begin());
        return;
    }
    if (ab.kind == EffectKind::CreatePersistent) {
        PersistentInstance p;
        p.x = o.tx;
        p.y = o.ty;
        p.radius = ab.radius;
        p.period = ab.period;
        p.timer = ab.period;
        p.amount = ab.amount;
        p.count = ab.period_count;
        p.owner = u.player;
        add_persistent(p);
        u.orders.erase(u.orders.begin());
        return;
    }
    if (ab.kind == EffectKind::CreateUnit) {
        if (ab.spawn_proto >= 0 && ab.spawn_proto < int32_t(spawn_protos_.size()) &&
            ab.spawn_count > 0)
            pending_spawns_.push_back({ab.spawn_proto, ab.spawn_count, u.x, u.y, u.player});
        u.orders.erase(u.orders.begin());
        return;
    }
    if (ab.kind == EffectKind::Morph) {
        if (ab.spawn_proto >= 0 && ab.spawn_proto < int32_t(spawn_protos_.size())) {
            Player& pl = player(u.player);
            if (pl.minerals >= ab.mineral_cost && pl.gas >= ab.gas_cost) {
                pl.minerals -= ab.mineral_cost;
                pl.gas -= ab.gas_cost;
                u.morph_proto = ab.spawn_proto;
                u.morph_timer = ab.duration;
                u.morph_heal_life_frac = ab.morph_heal_life_frac;
                u.morph_heal_shield_frac = ab.morph_heal_shield_frac;
            }
        }
        u.orders.erase(u.orders.begin());
        return;
    }

    const uint32_t tid = o.target;
    if (tid >= units_.size() || !units_[tid].alive) {
        u.orders.erase(u.orders.begin());
        return;
    }
    Unit& tgt = units_[tid];

    if (ab.range.raw() > 0) {
        Fixed reach = ab.range + u.radius + tgt.radius;
        if (dist2(u, tgt) > reach * reach) {
            move_unit_toward(u, tgt.x, tgt.y, move_step(u, dt));
            return;
        }
    }

    switch (ab.kind) {
        case EffectKind::Damage: {
            Fixed dmg = ab.amount;
            if (tgt.shields.raw() > 0) {
                if (tgt.shields >= dmg) {
                    tgt.shields = tgt.shields - dmg;
                    dmg = Fixed();
                } else {
                    dmg = dmg - tgt.shields;
                    tgt.shields = Fixed();
                }
            }
            if (dmg.raw() > 0) tgt.hp = tgt.hp - dmg;
            if (tgt.hp.raw() <= 0 && tgt.alive) {
                tgt.alive = false;
                award_kill_xp(u, tgt.kill_xp);
            }
            break;
        }
        case EffectKind::Heal: {
            tgt.hp = tgt.hp + ab.amount;
            if (tgt.hp > tgt.hp_max) tgt.hp = tgt.hp_max;
            break;
        }
        case EffectKind::Buff: {
            if (tgt.buff_timer.raw() <= 0) {
                tgt.wpn_damage = tgt.wpn_damage + ab.amount;
                tgt.buff_damage = ab.amount;
            }
            tgt.buff_timer = ab.duration;
            break;
        }
        case EffectKind::ApplyBehavior: {
            BehaviorInstance b;
            b.kind = kCastBehaviorKind;
            b.timer = ab.duration.raw() > 0 ? ab.duration : Fixed::from_raw(1);
            b.speed_mult = ab.move_speed_mult;
            b.root = ab.beh_root;
            b.stun = ab.beh_stun;
            b.dmg_per_tick = ab.beh_dmg_per_tick;
            b.dmg_period = ab.beh_dmg_period;
            b.dmg_timer = ab.beh_dmg_period;
            add_behavior(tid, b);
            if (ab.attack_speed_mult.raw() != Fixed::from_int(1).raw())
                tgt.atk_speed_mult = ab.attack_speed_mult;
            break;
        }
        case EffectKind::ModifyEnergy: {
            Fixed dmg = tgt.energy * ab.energy_fraction;
            tgt.energy = Fixed();
            if (tgt.shields.raw() > 0) {
                if (tgt.shields >= dmg) {
                    tgt.shields = tgt.shields - dmg;
                    dmg = Fixed();
                } else {
                    dmg = dmg - tgt.shields;
                    tgt.shields = Fixed();
                }
            }
            if (dmg.raw() > 0) tgt.hp = tgt.hp - dmg;
            if (tgt.hp.raw() <= 0 && tgt.alive) {
                tgt.alive = false;
                award_kill_xp(u, tgt.kill_xp);
            }
            break;
        }
        case EffectKind::Teleport:
        case EffectKind::CreatePersistent:
        case EffectKind::CreateUnit:
        case EffectKind::Morph:
            break;
    }
    u.orders.erase(u.orders.begin());
}

void World::award_kill_xp(Unit& killer, int64_t victim_xp) {
    if (killer.vet_index < 0 || killer.vet_index >= int32_t(vet_defs_.size()) ||
        victim_xp <= 0)
        return;
    const VeterancyDef& d = vet_defs_[killer.vet_index];
    killer.xp += (victim_xp * d.xp_fraction.raw()) >> Fixed::kShift;
    while (killer.level < d.max_level() &&
           killer.xp >= d.xp_threshold[killer.level + 1]) {
        int from = killer.level, to = killer.level + 1;
        Fixed dlife = d.cum_life[to] - d.cum_life[from];
        Fixed ddmg = d.cum_damage[to] - d.cum_damage[from];
        killer.hp_max = killer.hp_max + dlife;
        killer.hp = killer.hp + dlife;
        killer.wpn_damage = killer.wpn_damage + ddmg;
        killer.level = to;
    }
}

bool World::queue_train(uint32_t producer, const Unit& proto, int32_t mineral_cost,
                        int32_t gas_cost, Fixed build_time) {
    if (producer >= units_.size() || units_[producer].constructing) return false;
    if (const int32_t slots = units_[producer].train_slots; slots > 0) {
        int in_flight = 0;
        for (const Pending& p : pending_)
            if (p.producer == producer) ++in_flight;
        if (in_flight >= slots) return false;
    }
    int32_t pl = units_[producer].player;
    Player& pp = player(pl);
    pp.minerals -= mineral_cost;
    pp.gas -= gas_cost;
    Unit u = proto;
    u.player = pl;
    pending_.push_back(Pending{producer, u, build_time});
    return true;
}

bool World::issue_worker_build(uint32_t builder, Fixed x, Fixed y, const Unit& proto,
                               int32_t mineral_cost, int32_t gas_cost, Fixed build_time,
                               Fixed arrive) {
    if (builder >= units_.size() || !units_[builder].alive) return false;
    Player& pp = player(units_[builder].player);
    pp.minerals -= mineral_cost;
    pp.gas -= gas_cost;
    Order o;
    o.type = OrderType::Move;
    o.tx = x;
    o.ty = y;
    issue(builder, o, true);
    worker_builds_.push_back(WorkerBuild{builder, x, y, proto, build_time, arrive});
    return true;
}

void World::step_worker_builds() {
    for (size_t i = 0; i < worker_builds_.size();) {
        WorkerBuild& wb = worker_builds_[i];
        if (wb.builder >= units_.size() || !units_[wb.builder].alive) {
            worker_builds_.erase(worker_builds_.begin() + i);
            continue;
        }
        const Unit& b = units_[wb.builder];
        const Fixed dx = wb.x - b.x, dy = wb.y - b.y;
        if (dx * dx + dy * dy > wb.arrive * wb.arrive) {
            ++i;
            continue;
        }
        Unit u = wb.proto;
        u.x = wb.x;
        u.y = wb.y;
        u.speed = Fixed();
        const uint32_t id = add_unit(u);
        Fixed start_hp = Fixed::from_raw(u.hp_max.raw() / 10);
        if (start_hp.raw() <= 0) start_hp = Fixed::from_int(1);
        Fixed bt = wb.build_time.raw() > 0 ? wb.build_time : Fixed::from_int(1);
        start_construction(id, bt, start_hp);
        units_[id].needs_builder = true;
        units_[id].builder = wb.builder;
        units_[wb.builder].construct_target = id;
        units_[wb.builder].orders.clear();
        worker_builds_.erase(worker_builds_.begin() + i);
    }
}

bool World::assign_builder(uint32_t worker, uint32_t building) {
    if (worker >= units_.size() || building >= units_.size()) return false;
    if (!units_[worker].alive) return false;
    Unit& b = units_[building];
    if (!b.constructing || !b.needs_builder) return false;
    if (b.builder < units_.size() && units_[b.builder].construct_target == building)
        units_[b.builder].construct_target = kNoTarget;
    Order o;
    o.type = OrderType::Move;
    o.tx = b.x;
    o.ty = b.y;
    issue(worker, o);
    b.builder = worker;
    units_[worker].construct_target = building;
    return true;
}

bool World::queue_train_at(uint32_t producer, const Unit& proto, int32_t mineral_cost,
                           int32_t gas_cost, Fixed build_time, Fixed x, Fixed y) {
    if (producer >= units_.size() || units_[producer].constructing) return false;
    if (const int32_t slots = units_[producer].train_slots; slots > 0) {
        int in_flight = 0;
        for (const Pending& p : pending_)
            if (p.producer == producer) ++in_flight;
        if (in_flight >= slots) return false;
    }
    int32_t pl = units_[producer].player;
    Player& pp = player(pl);
    pp.minerals -= mineral_cost;
    pp.gas -= gas_cost;
    Unit u = proto;
    u.player = pl;
    Pending pd{producer, u, build_time};
    pd.has_pos = true;
    pd.px = x;
    pd.py = y;
    pending_.push_back(std::move(pd));
    return true;
}

void World::step_construction(Unit& u, Fixed dt) {
    if (u.needs_builder) {
        if (u.builder >= units_.size() || !units_[u.builder].alive ||
            units_[u.builder].construct_target != u.id) {
            u.builder = kNoTarget;
            return;
        }
        Unit& b = units_[u.builder];
        const Fixed dx = b.x - u.x, dy = b.y - u.y;
        const Fixed reach = u.radius + b.radius + Fixed::from_int(2);
        if (dx * dx + dy * dy > reach * reach) return;
        if (!b.orders.empty()) b.orders.clear();
        static const int OX[8] = {2, 1, 0, -1, -2, -1, 0, 1};
        static const int OY[8] = {0, 1, 2, 1, 0, -1, -2, -1};
        const int slot = int((tick() / 8) % 8);
        const Fixed off = u.radius + b.radius + Fixed::from_ratio(1, 4);
        b.x = u.x + Fixed::from_ratio(OX[slot], 2) * off;
        b.y = u.y + Fixed::from_ratio(OY[slot], 2) * off;
    }

    if (u.build_time.raw() <= 0) {
        u.build_progress = Fixed::from_int(1);
        u.hp = u.hp_max;
        u.constructing = false;
        if (u.builder < units_.size() && units_[u.builder].construct_target == u.id)
            units_[u.builder].construct_target = kNoTarget;
        u.builder = kNoTarget;
        u.needs_builder = false;
        return;
    }
    const Fixed dp = dt / u.build_time;
    u.build_progress = u.build_progress + dp;
    u.hp = u.hp + (u.hp_max - u.build_hp0) * dp;
    if (u.build_progress >= Fixed::from_int(1)) {
        u.build_progress = Fixed::from_int(1);
        u.hp = u.hp_max;
        u.constructing = false;
        if (u.builder < units_.size() && units_[u.builder].construct_target == u.id)
            units_[u.builder].construct_target = kNoTarget;
        u.builder = kNoTarget;
        u.needs_builder = false;
    } else if (u.hp > u.hp_max) {
        u.hp = u.hp_max;
    }
}

void World::step_auras() {
    bool any = false;
    for (const Unit& s : units_)
        if (s.alive && s.aura_dmg_reduction.raw() > 0 && s.aura_radius.raw() > 0) { any = true; break; }
    if (!any) return;
    for (Unit& u : units_) u.aura_reduction = Fixed();
    for (const Unit& s : units_) {
        if (!s.alive || s.aura_dmg_reduction.raw() <= 0 || s.aura_radius.raw() <= 0) continue;
        const Fixed r2 = s.aura_radius * s.aura_radius;
        for (Unit& u : units_) {
            if (!u.alive || !are_allied(u.player, s.player)) continue;
            const Fixed dx = u.x - s.x, dy = u.y - s.y;
            if (dx * dx + dy * dy > r2) continue;
            if (s.aura_dmg_reduction > u.aura_reduction) u.aura_reduction = s.aura_dmg_reduction;
        }
    }
}

void World::step_power() {
    bool any = false;
    for (const Unit& u : units_)
        if (u.alive && u.needs_power) { any = true; break; }
    if (!any) return;
    for (Unit& u : units_) {
        if (!u.alive || !u.needs_power) continue;
        u.powered = powered_at(u.x, u.y, u.player);
    }
}

void World::step_cloak_field() {
    bool any = false;
    for (const Unit& s : units_)
        if (s.alive && s.cloak_field_radius.raw() > 0) { any = true; break; }
    if (!any) return;
    for (Unit& u : units_) u.field_cloaked = false;
    for (const Unit& s : units_) {
        if (!s.alive || s.cloak_field_radius.raw() <= 0) continue;
        const Fixed r2 = s.cloak_field_radius * s.cloak_field_radius;
        for (Unit& u : units_) {
            if (!u.alive || !are_allied(u.player, s.player)) continue;
            const Fixed dx = u.x - s.x, dy = u.y - s.y;
            if (dx * dx + dy * dy <= r2) u.field_cloaked = true;
        }
    }
}

void World::step_bunker_fire(Fixed dt) {
    for (size_t ti = 0; ti < units_.size(); ++ti) {
        if (!units_[ti].alive || !units_[ti].attack_relay || units_[ti].cargo_space <= 0) continue;
        const Fixed bx = units_[ti].x, by = units_[ti].y, bonus = units_[ti].relay_range_bonus;
        for (size_t pi = 0; pi < units_.size(); ++pi) {
            Unit& p = units_[pi];
            if (!p.alive || !p.loaded || p.transport_id != uint32_t(ti)) continue;
            if (p.wpn_damage.raw() <= 0 || p.is_worker) continue;
            if (p.cooldown.raw() > 0) {
                p.cooldown = p.cooldown - dt;
                if (p.cooldown.raw() < 0) p.cooldown = Fixed();
                continue;
            }
            const Fixed reach = p.wpn_range + bonus;
            uint32_t best = kNoTarget;
            Fixed best_d2;
            for (size_t ei = 0; ei < units_.size(); ++ei) {
                const Unit& e = units_[ei];
                if (!e.alive || e.loaded || are_allied(e.player, p.player)) continue;
                const Fixed dx = e.x - bx, dy = e.y - by, r = reach + e.radius;
                const Fixed d2 = dx * dx + dy * dy;
                if (d2 > r * r) continue;
                if (best == kNoTarget || d2 < best_d2) { best = uint32_t(ei); best_d2 = d2; }
            }
            if (best == kNoTarget) continue;
            Unit& tgt = units_[best];
            Defender d{tgt.hp, tgt.shields, tgt.armor, tgt.damage_reduction + tgt.aura_reduction};
            d.shield_armor = tgt.shield_armor;
            apply_hit(Weapon{p.wpn_damage, p.wpn_period}, d);
            tgt.hp = d.life;
            tgt.shields = d.shields;
            p.cooldown = p.wpn_period;
            if (tgt.hp.raw() <= 0 && tgt.alive) {
                tgt.alive = false;
                award_kill_xp(p, tgt.kill_xp);
            }
        }
    }
}

void World::step_persistents(Fixed dt) {
    for (size_t i = 0; i < persistents_.size();) {
        PersistentInstance& p = persistents_[i];
        if (p.count <= 0) {
            persistents_.erase(persistents_.begin() + i);
            continue;
        }
        p.timer = p.timer - dt;
        if (p.timer.raw() <= 0) {
            p.timer = p.timer + p.period;
            for (Unit& u : units_) {
                if (!u.alive || u.player == p.owner) continue;
                const Fixed dx = u.x - p.x, dy = u.y - p.y;
                const Fixed reach = p.radius + u.radius;
                if (dx * dx + dy * dy > reach * reach) continue;
                if (p.amount.raw() > 0) {
                    Fixed dmg = p.amount;
                    if (u.shields.raw() > 0) {
                        if (u.shields >= dmg) {
                            u.shields = u.shields - dmg;
                            dmg = Fixed();
                        } else {
                            dmg = dmg - u.shields;
                            u.shields = Fixed();
                        }
                    }
                    if (dmg.raw() > 0) u.hp = u.hp - dmg;
                    if (u.hp.raw() <= 0 && u.alive) u.alive = false;
                }
                if (p.on_hit.timer.raw() > 0) add_behavior(u.id, p.on_hit);
            }
            p.count -= 1;
        }
        ++i;
    }
}

void World::step_production(Fixed dt) {
    for (size_t i = 0; i < pending_.size();) {
        Pending& pd = pending_[i];
        if (pd.producer < units_.size()) {
            const Unit& pr = units_[pd.producer];
            if (pr.needs_power && !pr.powered) { ++i; continue; }
        }
        Fixed adv = dt;
        if (pd.producer < units_.size() && units_[pd.producer].chrono_timer.raw() > 0)
            adv = adv + adv / Fixed::from_int(2);
        pd.timer = pd.timer - adv;
        if (pd.timer.raw() <= 0) {
            Unit u = pd.proto;
            bool rally = false;
            Fixed rx, ry;
            if (pd.has_pos) {
                u.x = pd.px;
                u.y = pd.py;
            } else if (pd.producer < units_.size()) {
                const Unit& pr = units_[pd.producer];
                u.x = pr.x + pr.radius + Fixed::from_int(1);
                u.y = pr.y;
                if (pr.has_rally) {
                    rally = true;
                    rx = pr.rally_x;
                    ry = pr.rally_y;
                }
            }
            uint32_t nid = add_unit(u);
            if (rally) {
                uint32_t node = kNoTarget;
                if (units_[nid].is_worker) {
                    Fixed best;
                    for (const Unit& o : units_) {
                        if (o.resource_left <= 0) continue;
                        const Fixed dx = o.x - rx, dy = o.y - ry, d = dx * dx + dy * dy;
                        const Fixed near = o.radius + Fixed::from_int(2);
                        if (d <= near * near && (node == kNoTarget || d < best)) {
                            node = o.id;
                            best = d;
                        }
                    }
                }
                if (node != kNoTarget) {
                    units_[nid].gather_target = node;
                    units_[nid].home_depot = pd.producer;
                    issue(nid, Order{OrderType::Gather, Fixed(), Fixed(), kNoTarget}, false);
                } else {
                    Order o;
                    o.type = OrderType::Move;
                    o.tx = rx;
                    o.ty = ry;
                    issue(nid, o, false);
                }
            }
            pending_.erase(pending_.begin() + i);
        } else {
            ++i;
        }
    }
}

void World::step_gather(Unit& u, Fixed dt) {
    if (u.mine_cd.raw() > 0) {
        u.mine_cd = u.mine_cd - dt;
        if (u.mine_cd.raw() <= 0) {
            u.mine_cd = Fixed();
            if (u.gather_target < units_.size()) {
                Unit& m = units_[u.gather_target];
                int32_t amt = m.yield_amount > 0 ? m.yield_amount
                                                 : (u.carry_amount > 0 ? u.carry_amount : 5);
                if (m.resource_left < amt) amt = m.resource_left;
                u.carrying = amt;
                m.resource_left -= amt;
            }
        }
        return;
    }

    if (u.return_cd.raw() > 0) {
        u.return_cd = u.return_cd - dt;
        return;
    }

    if (u.gather_target == kNoTarget || u.gather_target >= units_.size() ||
        units_[u.gather_target].resource_left <= 0) {
        uint32_t best = kNoTarget;
        Fixed bd;
        for (const Unit& o : units_) {
            if (o.resource_left <= 0) continue;
            if (o.is_gas != u.gathers_gas) continue;
            Fixed d = dist2(u, o);
            if (best == kNoTarget || d < bd) {
                best = o.id;
                bd = d;
            }
        }
        u.gather_target = best;
    }

    if (u.carrying > 0) {
        if (u.home_depot >= units_.size() || !units_[u.home_depot].alive) return;
        Unit& d = units_[u.home_depot];
        Fixed reach = u.radius + d.radius + Fixed::from_ratio(1, 4);
        if (dist2(u, d) <= reach * reach) {
            if (u.gathers_gas) player(u.player).gas += u.carrying;
            else player(u.player).minerals += u.carrying;
            u.carrying = 0;
            u.return_cd = u.return_delay;
        } else {
            move_unit_toward(u, d.x, d.y, move_step(u, dt));
        }
    } else if (u.gather_target < units_.size()) {
        Unit& m = units_[u.gather_target];
        Fixed reach = u.radius + m.radius + Fixed::from_ratio(1, 4);
        if (dist2(u, m) <= reach * reach) {
            if (m.miners_max > 0) {
                int active = 0;
                for (const Unit& w : units_)
                    if (w.alive && w.id != u.id && w.gather_target == u.gather_target &&
                        w.mine_cd.raw() > 0)
                        ++active;
                if (active >= m.miners_max) return;
            }
            u.mine_cd = u.mine_time.raw() > 0 ? u.mine_time : Fixed::parse("2.786");
        } else {
            move_unit_toward(u, m.x, m.y, move_step(u, dt));
        }
    }
}

void World::step() {
    const Fixed dt = World::dt() * game_speed_;
    creep_.tick();
    step_power();
    step_production(dt);
    step_worker_builds();
    step_persistents(dt);
    for (size_t i = 0; i < stat_mods_.size();) {
        StatModInstance& m = stat_mods_[i];
        m.timer = m.timer - dt;
        if (m.timer.raw() > 0) { ++i; continue; }
        if (m.target < units_.size()) apply_stat_delta(units_[m.target], m.stat, Fixed() - m.delta);
        stat_mods_.erase(stat_mods_.begin() + i);
    }
    for (size_t i = 0; i < knockbacks_.size();) {
        KnockImpulse& im = knockbacks_[i];
        if (im.target >= units_.size() || !units_[im.target].alive) {
            knockbacks_.erase(knockbacks_.begin() + i);
            continue;
        }
        Fixed step = im.speed * dt;
        if (step >= im.remaining) step = im.remaining;
        Unit& u = units_[im.target];
        u.x = u.x + im.dx * step;
        u.y = u.y + im.dy * step;
        u.path.clear();
        u.path_next = 0;
        u.path_gx = INT32_MIN;
        u.path_gy = INT32_MIN;
        im.remaining = im.remaining - step;
        if (im.remaining.raw() <= 0) knockbacks_.erase(knockbacks_.begin() + i);
        else ++i;
    }
    for (size_t i = 0; i < force_fields_.size();) {
        ForceFieldInstance& ff = force_fields_[i];
        ff.timer = ff.timer - dt;
        if (ff.timer.raw() <= 0) {
            for (const auto& c : ff.cells) nav_.set_blocked(c.first, c.second, false);
            force_fields_.erase(force_fields_.begin() + i);
        } else {
            ++i;
        }
    }
    step_auras();
    step_cloak_field();
    for (Unit& u : units_) {
        if (!u.alive) continue;

        if (u.life_regen.raw() > 0 && u.hp.raw() > 0 && u.hp < u.hp_max) {
            u.hp = u.hp + u.life_regen * dt;
            if (u.hp > u.hp_max) u.hp = u.hp_max;
        }
        if (u.shield_regen_rate.raw() > 0 && u.shields_max.raw() > 0) {
            const Fixed total = u.hp + u.shields;
            if (total < u.shield_last_total) u.shield_since_hit = Fixed();
            if (u.shield_since_hit < u.shield_regen_delay) {
                u.shield_since_hit = u.shield_since_hit + dt;
            } else if (u.shields < u.shields_max) {
                u.shields = u.shields + u.shield_regen_rate * dt;
                if (u.shields > u.shields_max) u.shields = u.shields_max;
            }
            u.shield_last_total = u.hp + u.shields;
        }
        if (u.time_life.raw() > 0) {
            u.time_life = u.time_life - dt;
            if (u.time_life.raw() <= 0) {
                u.time_life = Fixed();
                u.alive = false;
                continue;
            }
        }
        if (u.chrono_timer.raw() > 0) {
            u.chrono_timer = u.chrono_timer - dt;
            if (u.chrono_timer.raw() < 0) u.chrono_timer = Fixed();
        }
        if (u.warp_cd.raw() > 0) {
            u.warp_cd = u.warp_cd - dt;
            if (u.warp_cd.raw() < 0) u.warp_cd = Fixed();
        }
        if (u.cloaked && u.cloak_drain.raw() > 0) {
            u.energy = u.energy - u.cloak_drain * dt;
            if (u.energy.raw() <= 0) {
                u.energy = Fixed();
                u.cloaked = false;
            }
        }

        if (u.morph_proto >= 0) {
            u.morph_timer = u.morph_timer - dt;
            if (u.morph_timer.raw() <= 0 && u.morph_proto < int32_t(spawn_protos_.size())) {
                const uint32_t keep_id = u.id;
                const int32_t keep_player = u.player;
                const Fixed keep_x = u.x, keep_y = u.y;
                const Fixed keep_hp = u.hp, keep_shields = u.shields;
                const Fixed heal_life = u.morph_heal_life_frac, heal_shield = u.morph_heal_shield_frac;
                u = spawn_protos_[u.morph_proto];
                u.id = keep_id;
                u.player = keep_player;
                u.x = keep_x;
                u.y = keep_y;
                u.hp = keep_hp < u.hp_max ? keep_hp : u.hp_max;
                u.shields = keep_shields < u.shields_max ? keep_shields : u.shields_max;
                if (heal_life.raw() > 0) {
                    u.hp = u.hp + u.hp_max * heal_life;
                    if (u.hp > u.hp_max) u.hp = u.hp_max;
                }
                if (heal_shield.raw() > 0) {
                    u.shields = u.shields + u.shields_max * heal_shield;
                    if (u.shields > u.shields_max) u.shields = u.shields_max;
                }
            }
            continue;
        }

        if (u.loaded && u.transport_id != kNoTarget) {
            const uint32_t t = u.transport_id;
            if (t >= units_.size() || !units_[t].alive) {
                if (t < units_.size() && units_[t].cargo_death) u.alive = false;
                else { u.loaded = false; u.transport_id = kNoTarget; }
                continue;
            }
        }

        if (u.loaded || u.burrowed) continue;

        if (u.constructing) {
            step_construction(u, dt);
            continue;
        }

        if (u.energy_max.raw() > 0 && u.energy < u.energy_max) {
            u.energy = u.energy + u.energy_regen * dt;
            if (u.energy > u.energy_max) u.energy = u.energy_max;
        }

        if (u.spawner_proto >= 0 && u.spawner_cap > 0 && u.spawner_period.raw() > 0) {
            const bool under_hard = u.spawner_hard_cap <= 0 || larva_total(u.id) < u.spawner_hard_cap;
            if (spawner_brood(u.id) < u.spawner_cap && under_hard) {
                u.spawner_timer = u.spawner_timer - dt;
                if (u.spawner_timer.raw() <= 0) {
                    pending_spawns_.push_back(
                        {u.spawner_proto, 1, u.x, u.y, u.player, u.id, false});
                    u.spawner_timer = u.spawner_period;
                }
            }
        }

        if (u.inject_proto >= 0 && u.inject_count > 0) {
            u.inject_timer = u.inject_timer - dt;
            if (u.inject_timer.raw() <= 0) {
                int n = u.inject_count;
                if (u.spawner_hard_cap > 0) {
                    const int room = u.spawner_hard_cap - larva_total(u.id);
                    n = room < n ? room : n;
                }
                if (n > 0)
                    pending_spawns_.push_back(
                        {u.inject_proto, n, u.x, u.y, u.player, u.id, true});
                u.inject_proto = -1;
                u.inject_count = 0;
            }
        }

        if (!u.behaviors.empty()) {
            for (BehaviorInstance& b : u.behaviors) {
                b.timer = b.timer - dt;
                if (b.dmg_per_tick.raw() > 0 && b.dmg_period.raw() > 0 && u.alive) {
                    b.dmg_timer = b.dmg_timer - dt;
                    if (b.dmg_timer.raw() <= 0) {
                        b.dmg_timer = b.dmg_period;
                        Fixed d = b.dmg_per_tick;
                        if (u.shields.raw() > 0) {
                            if (u.shields >= d) { u.shields = u.shields - d; d = Fixed(); }
                            else { d = d - u.shields; u.shields = Fixed(); }
                        }
                        if (d.raw() > 0) u.hp = u.hp - d;
                        if (u.hp.raw() <= 0) u.alive = false;
                    }
                }
            }
            u.behaviors.erase(
                std::remove_if(u.behaviors.begin(), u.behaviors.end(),
                               [](const BehaviorInstance& b) { return b.timer.raw() <= 0; }),
                u.behaviors.end());
        }

        if (u.buff_timer.raw() > 0) {
            u.buff_timer = u.buff_timer - dt;
            if (u.buff_timer.raw() <= 0) {
                u.buff_timer = Fixed();
                u.wpn_damage = u.wpn_damage - u.buff_damage;
                u.buff_damage = Fixed();
            }
        }

        if (behavior_stunned(u)) continue;

        if (!u.orders.empty() && u.orders.front().type == OrderType::Gather) {
            const Order& go = u.orders.front();
            if (go.target < units_.size() && units_[go.target].resource_left > 0)
                u.gather_target = go.target;
            step_gather(u, dt);
            continue;
        }

        if (!u.orders.empty() && u.orders.front().type == OrderType::Cast) {
            step_cast(u, dt);
            continue;
        }

        if (u.cooldown.raw() > 0) {
            u.cooldown = u.cooldown - dt;
            if (u.cooldown.raw() < 0) u.cooldown = Fixed();
        }

        while (!u.orders.empty()) {
            Order& o = u.orders.front();
            bool done = false;
            if (o.type == OrderType::Attack)
                done = (o.target >= units_.size() || !units_[o.target].alive);
            else if (o.type == OrderType::Move || o.type == OrderType::AttackMove) {
                Fixed dx = o.tx - u.x, dy = o.ty - u.y;
                Fixed near = u.radius + Fixed::from_ratio(1, 8);
                done = (dx * dx + dy * dy) <= near * near;
            }
            if (!done) break;
            u.orders.erase(u.orders.begin());
        }

        OrderType mode =
            u.orders.empty() ? OrderType::Idle : u.orders.front().type;

        uint32_t atk = kNoTarget;
        bool can_move = true, has_dest = false;
        Fixed dx_dest, dy_dest;

        switch (mode) {
            case OrderType::Move:
                can_move = true;
                has_dest = true;
                dx_dest = u.orders.front().tx;
                dy_dest = u.orders.front().ty;
                break;
            case OrderType::Attack:
                atk = u.orders.front().target;
                break;
            case OrderType::HoldPosition:
                can_move = false;
                atk = acquire_target(u);
                break;
            case OrderType::AttackMove:
                atk = acquire_target(u);
                if (atk == kNoTarget) {
                    has_dest = true;
                    dx_dest = u.orders.front().tx;
                    dy_dest = u.orders.front().ty;
                }
                break;
            case OrderType::Patrol: {
                atk = acquire_target(u);
                if (atk == kNoTarget) {
                    Order& o = u.orders.front();
                    const Fixed ddx = o.tx - u.x, ddy = o.ty - u.y;
                    const Fixed near = u.radius + Fixed::from_ratio(1, 8);
                    if (ddx * ddx + ddy * ddy <= near * near) {
                        const Fixed sx = o.tx, sy = o.ty;
                        o.tx = o.px; o.ty = o.py;
                        o.px = sx; o.py = sy;
                    }
                    has_dest = true;
                    dx_dest = o.tx;
                    dy_dest = o.ty;
                }
                break;
            }
            case OrderType::Idle:
            default:
                atk = acquire_target(u);
                break;
        }

        if (u.hold_fire) atk = kNoTarget;

        if (behavior_rooted(u)) can_move = false;

        u.target = atk;

        if (atk != kNoTarget && atk < units_.size() && units_[atk].alive) {
            Unit& tgt = units_[atk];
            Fixed w_dmg = u.wpn_damage, w_per = u.wpn_period, w_rng = u.wpn_range;
            const uint32_t plane_bit = tgt.flying ? 2u : 1u;
            if (!(u.wpn_target_filter & plane_bit) && u.wpn2_damage.raw() > 0 &&
                (u.wpn2_target_filter & plane_bit)) {
                w_dmg = u.wpn2_damage;
                w_per = u.wpn2_period;
                w_rng = u.wpn2_range;
            }
            if (u.player >= 0) {
                const int32_t wu = player(u.player).weapon_upgrade;
                if (wu != 0) w_dmg = w_dmg + Fixed::from_int(wu);
            }
            Fixed reach = w_rng + u.radius + tgt.radius;
            bool in_range = dist2(u, tgt) <= reach * reach;
            if (in_range) {
                if (u.cooldown.raw() == 0 && w_dmg.raw() > 0) {
                    Defender d{tgt.hp, tgt.shields, tgt.armor, tgt.damage_reduction + tgt.aura_reduction};
                    d.shield_armor = tgt.shield_armor;
                    apply_hit(Weapon{w_dmg, w_per}, d);
                    tgt.hp = d.life;
                    tgt.shields = d.shields;
                    if (tgt.hp.raw() <= 0 && tgt.alive) {
                        tgt.alive = false;
                        award_kill_xp(u, tgt.kill_xp);
                    }
                    if (u.splash_outer.raw() > 0) {
                        const Fixed tx = tgt.x, ty = tgt.y;
                        for (Unit& o : units_) {
                            if (!o.alive || o.id == atk || are_allied(o.player, u.player)) continue;
                            if (o.loaded || o.burrowed) continue;
                            const Fixed dx = o.x - tx, dy = o.y - ty;
                            const Fixed d2 = dx * dx + dy * dy;
                            Fixed frac;
                            if (d2 <= u.splash_inner * u.splash_inner) frac = Fixed::from_int(1);
                            else if (d2 <= u.splash_mid * u.splash_mid) frac = Fixed::from_ratio(1, 2);
                            else if (d2 <= u.splash_outer * u.splash_outer) frac = Fixed::from_ratio(1, 4);
                            else continue;
                            Defender sd{o.hp, o.shields, o.armor, o.damage_reduction + o.aura_reduction};
                            sd.shield_armor = o.shield_armor;
                            apply_hit(Weapon{w_dmg * frac, w_per}, sd);
                            o.hp = sd.life;
                            o.shields = sd.shields;
                            if (o.hp.raw() <= 0 && o.alive) {
                                o.alive = false;
                                award_kill_xp(u, o.kill_xp);
                            }
                        }
                    }
                    u.cooldown = u.atk_speed_mult.raw() == Fixed::from_int(1).raw()
                                     ? w_per
                                     : w_per / u.atk_speed_mult;
                }
            } else if (can_move) {
                move_unit_toward(u, tgt.x, tgt.y, move_step(u, dt));
            }
        } else if (has_dest && can_move) {
            move_unit_toward(u, dx_dest, dy_dest, move_step(u, dt));
        }
    }
    step_bunker_fire(dt);

    if (!pending_spawns_.empty()) {
        static const int DX[8] = {1, 1, 0, -1, -1, -1, 0, 1};
        static const int DY[8] = {0, 1, 1, 1, 0, -1, -1, -1};
        for (const PendingSpawn& ps : pending_spawns_) {
            for (int i = 0; i < ps.count; ++i) {
                Unit s = spawn_protos_[ps.proto];
                s.player = ps.owner;
                s.spawn_parent = ps.parent;
                s.larva_injected = ps.injected;
                s.x = ps.x + Fixed::from_int(DX[i & 7]);
                s.y = ps.y + Fixed::from_int(DY[i & 7]);
                s.alive = true;
                add_unit(s);
            }
        }
        pending_spawns_.clear();
    }

    apply_separation();
    ++tick_;
}

uint64_t World::state_hash() const {
    auto mix = [](uint64_t h, uint64_t v) {
        h ^= v + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
        return h;
    };
    uint64_t h = 1469598103934665603ull;
    h = mix(h, uint64_t(tick_));
    h = mix(h, rng_.state());
    if (fog_acquire_) h = mix(h, 0xF06Au);
    if (game_speed_.raw() != Fixed::from_int(1).raw()) h = mix(h, uint32_t(game_speed_.raw()));
    for (const StatModInstance& m : stat_mods_) {
        h = mix(h, m.target);
        h = mix(h, uint32_t(m.stat));
        h = mix(h, uint32_t(m.delta.raw()));
        h = mix(h, uint32_t(m.timer.raw()));
    }
    for (const KnockImpulse& im : knockbacks_) {
        h = mix(h, im.target);
        h = mix(h, uint32_t(im.remaining.raw()));
    }
    for (const ForceFieldInstance& ff : force_fields_) {
        h = mix(h, uint32_t(ff.x.raw()));
        h = mix(h, uint32_t(ff.y.raw()));
        h = mix(h, uint32_t(ff.timer.raw()));
    }
    for (const Unit& u : units_) {
        h = mix(h, u.id);
        h = mix(h, uint32_t(u.player));
        h = mix(h, uint32_t(u.x.raw()));
        h = mix(h, uint32_t(u.y.raw()));
        h = mix(h, uint32_t(u.hp.raw()));
        h = mix(h, uint32_t(u.shields.raw()));
        h = mix(h, uint32_t(u.cooldown.raw()));
        h = mix(h, u.target);
        h = mix(h, u.alive ? 1u : 0u);
        h = mix(h, uint32_t(u.resource_left));
        h = mix(h, uint32_t(u.carrying));
        h = mix(h, uint32_t(u.mine_cd.raw()));
        if (u.return_cd.raw() != 0) h = mix(h, uint32_t(u.return_cd.raw()));
        h = mix(h, u.gather_target);
        if (u.is_gas) h = mix(h, 0x6A5Eu);
        if (u.gathers_gas) h = mix(h, 0x6A5Fu);
        h = mix(h, uint64_t(u.xp));
        h = mix(h, uint32_t(u.level));
        h = mix(h, uint32_t(u.hp_max.raw()));
        if (u.buff_timer.raw() != 0) {
            h = mix(h, uint32_t(u.buff_timer.raw()));
            h = mix(h, uint32_t(u.buff_damage.raw()));
        }
        if (u.constructing) {
            h = mix(h, uint32_t(u.build_progress.raw()));
            h = mix(h, uint32_t(u.build_time.raw()));
            if (u.needs_builder) h = mix(h, u.builder);
        }
        if (u.construct_target != kNoTarget) h = mix(h, u.construct_target);
        if (u.energy_max.raw() != 0) h = mix(h, uint32_t(u.energy.raw()));
        if (u.cloaked) h = mix(h, 0xC10Au);
        if (u.hold_fire) h = mix(h, 0x40F1u);
        if (u.chrono_timer.raw() != 0) h = mix(h, uint32_t(u.chrono_timer.raw()));
        if (u.shields_max.raw() != 0) h = mix(h, uint32_t(u.shields_max.raw()));
        if (u.shield_regen_rate.raw() > 0 && u.shields_max.raw() > 0)
            h = mix(h, uint32_t(u.shield_since_hit.raw()));
        if (u.atk_speed_mult.raw() != Fixed::from_int(1).raw())
            h = mix(h, uint32_t(u.atk_speed_mult.raw()));
        if (u.damage_reduction.raw() != 0) h = mix(h, uint32_t(u.damage_reduction.raw()));
        if (u.aura_reduction.raw() != 0) h = mix(h, uint32_t(u.aura_reduction.raw()));
        if (u.needs_power) h = mix(h, u.powered ? 0x50E1u : 0x50E0u);
        if (u.loaded) { h = mix(h, 0x10ADu); h = mix(h, u.transport_id); }
        if (u.burrowed) h = mix(h, 0xB020u);
        if (u.field_cloaked) h = mix(h, 0xC10Au);
        if (u.time_life.raw() > 0) h = mix(h, uint32_t(u.time_life.raw()));
        if (u.morph_proto >= 0) {
            h = mix(h, uint32_t(u.morph_timer.raw()));
            h = mix(h, uint32_t(u.morph_proto));
            h = mix(h, uint32_t(u.morph_heal_life_frac.raw()));
            h = mix(h, uint32_t(u.morph_heal_shield_frac.raw()));
        }
        if (u.spawner_proto >= 0) {
            h = mix(h, uint32_t(u.spawner_timer.raw()));
            h = mix(h, uint32_t(u.spawner_cap));
        }
        if (u.spawn_parent != kNoTarget) {
            h = mix(h, u.spawn_parent);
            if (u.larva_injected) h = mix(h, 0x1A79Au);
        }
        if (u.inject_proto >= 0) {
            h = mix(h, uint32_t(u.inject_timer.raw()));
            h = mix(h, uint32_t(u.inject_count));
        }
        if (u.warp_cd.raw() > 0) h = mix(h, uint32_t(u.warp_cd.raw()));
        for (const BehaviorInstance& b : u.behaviors) {
            h = mix(h, b.kind);
            h = mix(h, uint32_t(b.timer.raw()));
            h = mix(h, uint32_t(b.speed_mult.raw()));
            h = mix(h, uint32_t((b.root ? 1u : 0u) | (b.stun ? 2u : 0u)));
        }
    }
    for (const Player& p : players_) {
        h = mix(h, uint32_t(p.minerals));
        h = mix(h, uint32_t(p.gas));
        if (p.team != -1) h = mix(h, uint32_t(p.team));
        for (size_t i = 0; i < p.upgrades.size(); ++i)
            if (p.upgrades[i] != 0) {
                h = mix(h, uint32_t(i));
                h = mix(h, uint32_t(p.upgrades[i]));
            }
        if (p.weapon_upgrade != 0) h = mix(h, uint32_t(p.weapon_upgrade) ^ 0x5717u);
    }
    for (const Pending& pd : pending_) {
        h = mix(h, pd.producer);
        h = mix(h, uint32_t(pd.timer.raw()));
        if (pd.has_pos) {
            h = mix(h, uint32_t(pd.px.raw()));
            h = mix(h, uint32_t(pd.py.raw()));
        }
    }
    for (const WorkerBuild& wb : worker_builds_) {
        h = mix(h, wb.builder);
        h = mix(h, uint32_t(wb.x.raw()));
        h = mix(h, uint32_t(wb.y.raw()));
    }
    for (const PersistentInstance& p : persistents_) {
        h = mix(h, uint32_t(p.x.raw()));
        h = mix(h, uint32_t(p.y.raw()));
        h = mix(h, uint32_t(p.timer.raw()));
        h = mix(h, uint32_t(p.count));
    }
    h = mix(h, creep_.checksum());
    return h;
}

}
