#include "sc2/sim/world.hpp"

#include <algorithm>

#include "sc2/sim/combat.hpp"
#include "sc2/sim/weapon.hpp"

namespace sc2::sim {

static const Fixed kBurnDownPeriod = Fixed::parse("0.3332");

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
        const bool by_team = are_allied(a.player, player);
        const bool by_grant = a.vision_player >= 0 && are_allied(a.vision_player, player);
        if (!by_team && !by_grant) continue;
        const Fixed dx = target.x - a.x, dy = target.y - a.y;
        if (dx * dx + dy * dy <= a.sight * a.sight) return true;
    }
    return false;
}

void World::update_watchtowers() {
    for (Watchtower& wt : watchtowers_) {
        if (wt.tower_id >= units_.size()) continue;
        Unit& tower = units_[wt.tower_id];
        if (!tower.alive) {
            tower.vision_player = -1;
            wt.owner = -1;
            continue;
        }
        bool incumbent_present = false;
        int32_t first = -1;
        for (const Unit& u : units_) {
            if (!u.alive || u.flying || u.loaded || u.burrowed || u.player < 0 ||
                u.speed.raw() <= 0)
                continue;
            const Fixed dx = u.x - tower.x, dy = u.y - tower.y;
            if (dx * dx + dy * dy > wt.capture_r2) continue;
            if (u.player == wt.owner) incumbent_present = true;
            if (first < 0 || u.player < first) first = u.player;
        }
        const int32_t new_owner = incumbent_present ? wt.owner : first;
        wt.owner = new_owner;
        tower.vision_player = new_owner;
    }
}

void World::return_dead_carry() {
    for (Unit& u : units_) {
        if (u.alive || u.carrying <= 0) continue;
        if (u.carry_source < units_.size()) {
            Unit& node = units_[u.carry_source];
            if (node.alive) node.resource_left += u.carrying;
        }
        u.carrying = 0;
    }
}

void World::update_destructibles() {
    for (Destructible& d : destructibles_) {
        if (d.cleared) continue;
        if (d.id < units_.size() && units_[d.id].alive && !units_[d.id].flying &&
            !units_[d.id].lowered)
            continue;
        for (const auto& c : d.cells) nav_.set_blocked(c.first, c.second, false);
        d.cleared = true;
    }
}

void World::update_conjoined() {
    for (Conjoined& g : conjoined_) {
        if (g.collapsed) continue;
        bool any_dead = false;
        for (uint32_t id : g.members)
            if (id < units_.size() && !units_[id].alive) {
                any_dead = true;
                break;
            }
        if (!any_dead) continue;
        for (uint32_t id : g.members) {
            if (id >= units_.size()) continue;
            units_[id].alive = false;
            units_[id].hp = Fixed();
            if (g.debris_proto >= 0 && g.debris_proto < int32_t(spawn_protos_.size())) {
                const Fixed mx = units_[id].x, my = units_[id].y;
                Unit debris = spawn_protos_[g.debris_proto];
                debris.x = mx;
                debris.y = my;
                debris.alive = true;
                add_unit(debris);
                if (debris.crush_damage.raw() > 0 && debris.crush_radius.raw() > 0) {
                    PersistentInstance rc;
                    rc.x = mx;
                    rc.y = my;
                    rc.radius = debris.crush_radius;
                    rc.amount = debris.crush_damage;
                    rc.period = World::dt();
                    rc.timer = World::dt();
                    rc.count = 8;
                    rc.owner = debris.player;
                    add_persistent(rc);
                }
            }
        }
        g.collapsed = true;
    }
}

uint32_t World::acquire_target(const Unit& u, Fixed max_range) const {
    uint32_t best = kNoTarget;
    Fixed best_d2;
    Fixed best_key;
    int32_t best_pri = 0;
    const bool sort_on = target_sort_ && u.target_sort != kTargetSortDefault;
    auto sort_key = [&](const Unit& o) -> Fixed {
        if (!sort_on) return Fixed();
        if (u.target_sort == kTargetSortLifeFractionLow)
            return o.hp_max.raw() > 0 ? o.hp / o.hp_max : Fixed::from_int(1);
        return Fixed();
    };
    const bool bounded = max_range.raw() > 0;
    for (const Unit& o : units_) {
        if (!o.alive || are_allied(o.player, u.player)) continue;
        if (o.resource_left > 0) continue;
        if (o.loaded) continue;
        if ((o.burrowed || o.cloaked_effective()) && !detected_by(u.player, o)) continue;
        const uint32_t plane_bit = o.flying ? 2u : 1u;
        const bool w1 = (u.wpn_target_filter & plane_bit) != 0;
        const bool w2 = u.wpn2_damage.raw() > 0 && (u.wpn2_target_filter & plane_bit) != 0;
        if (!w1 && !w2) continue;
        if (u.zone_radius.raw() > 0) {
            if (o.flying) continue;
            const Fixed zx = o.x - u.zone_x, zy = o.y - u.zone_y;
            if (zx * zx + zy * zy > u.zone_radius * u.zone_radius) continue;
        }
        if (fog_acquire_ && !visible_to(u.player, o)) continue;
        Fixed d2 = dist2(u, o);
        {
            const bool w1_fire = w1 && (u.wpn_min_range.raw() <= 0 || d2 >= u.wpn_min_range * u.wpn_min_range);
            const bool w2_fire = w2 && (u.wpn2_min_range.raw() <= 0 || d2 >= u.wpn2_min_range * u.wpn2_min_range);
            if (!w1_fire && !w2_fire) continue;
        }
        if (bounded) {
            const Fixed reach = max_range + u.radius + o.radius;
            if (d2 > reach * reach) continue;
        }
        const Fixed key = sort_key(o);
        const int32_t pri = target_priority_ ? o.target_priority : 0;
        const bool better = best == kNoTarget || pri > best_pri ||
                            (pri == best_pri &&
                             (key < best_key || (key == best_key && d2 < best_d2)));
        if (better) {
            best = o.id;
            best_d2 = d2;
            best_key = key;
            best_pri = pri;
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

Fixed World::move_step(Unit& u, Fixed dt) {
    Fixed speed = u.speed;
    if (creep_.is_creep(u.x, u.y)) speed = speed * u.speed_mult_creep;
    if (!u.behaviors.empty()) speed = speed * behavior_speed_mult(u);
    if (!acceleration_ || u.accel.raw() <= 0) return speed * dt;
    u.moving_now = true;
    if (u.cur_speed < speed) {
        u.cur_speed = u.cur_speed + u.accel * dt;
        if (u.cur_speed > speed) u.cur_speed = speed;
    } else if (u.cur_speed > speed) {
        const Fixed d = u.decel.raw() > 0 ? u.decel : u.accel;
        u.cur_speed = u.cur_speed - d * dt;
        if (u.cur_speed < speed) u.cur_speed = speed;
    }
    return u.cur_speed * dt;
}

bool World::advance_turn(Unit& u, Fixed tx, Fixed ty) {
    if (!turn_limit_) return true;
    const Fixed dx = tx - u.x, dy = ty - u.y;
    if (dx.raw() == 0 && dy.raw() == 0) return true;
    const int32_t kPi = trig_detail::kPi, kTwoPi = trig_detail::kTwoPi;
    auto wrap = [&](Fixed a) {
        int32_t r = a.raw() % kTwoPi;
        if (r > kPi) r -= kTwoPi;
        else if (r < -kPi) r += kTwoPi;
        return Fixed::from_raw(r);
    };
    const Fixed desired = fixed_atan2(dy, dx);
    const Fixed err = wrap(desired - u.facing);
    if (arc_movement_ && acceleration_ && u.cur_speed.raw() > 0 && u.lateral_accel.raw() > 0) {
        Fixed budget = (u.lateral_accel * World::dt()) / u.cur_speed;
        if (u.turn_rate.raw() > 0 && u.turn_rate < Fixed::from_int(int(kInstantTurn))) {
            const Fixed rate_budget = (u.turn_rate * World::dt()) * Fixed::from_raw(kPi) / Fixed::from_int(180);
            if (rate_budget.raw() > 0 && rate_budget < budget) budget = rate_budget;
        }
        if (budget.raw() > 0) {
            const int32_t mag = err.raw() >= 0 ? err.raw() : -err.raw();
            u.facing = (mag <= budget.raw()) ? desired
                                             : wrap(u.facing + (err.raw() >= 0 ? budget : Fixed() - budget));
        }
        return true;
    }
    const Fixed rate = (acceleration_ && u.cur_speed.raw() == 0) ? u.stationary_turn_rate : u.turn_rate;
    if (rate.raw() <= 0) return true;
    if (rate >= Fixed::from_int(int(kInstantTurn))) return true;
    const Fixed budget = (rate * World::dt()) * Fixed::from_raw(kPi) / Fixed::from_int(180);
    if (budget.raw() <= 0) return true;
    const int32_t mag = err.raw() >= 0 ? err.raw() : -err.raw();
    if (mag <= budget.raw()) {
        u.facing = desired;
    } else {
        u.facing = wrap(u.facing + (err.raw() >= 0 ? budget : Fixed() - budget));
    }
    const int32_t rem_mag = [&] { const int32_t r = wrap(desired - u.facing).raw(); return r >= 0 ? r : -r; }();
    return rem_mag <= kPi / 16;
}

bool World::move_unit_toward(Unit& u, Fixed gx, Fixed gy, Fixed step) {
    u.moved_this_tick = true;
    if (!nav_.has_blocked() || u.flying) {
        if (!advance_turn(u, gx, gy)) return false;
        if (arc_movement_ && acceleration_ && u.cur_speed.raw() > 0 && u.lateral_accel.raw() > 0) {
            const Fixed dgx = gx - u.x, dgy = gy - u.y;
            if (dgx * dgx + dgy * dgy <= step * step) {
                u.x = gx;
                u.y = gy;
                return true;
            }
            Fixed c, s;
            fixed_sincos(u.facing, c, s);
            u.x = u.x + c * step;
            u.y = u.y + s * step;
            return false;
        }
        return move_toward(u, gx, gy, step);
    }

    int scx, scy, gcx, gcy;
    nav_.to_cell(u.x, u.y, scx, scy);
    nav_.to_cell(gx, gy, gcx, gcy);

    const bool need_plan = u.path.empty() || u.path_next >= u.path.size() ||
                           u.path_gx != gcx || u.path_gy != gcy;
    if (need_plan) {
        std::vector<NavCell> cells;
        find_path(nav_, scx, scy, gcx, gcy, cells, u.can_jump, u.crushes_ff, u.walks_cliffs);
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
        if (!advance_turn(u, wp.x, wp.y)) return false;
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
            if (a.flying || b.flying) continue;
            if (a.burrowed || b.burrowed) continue;
            if (a.collide_flags != 0 && b.collide_flags != 0 &&
                (a.collide_flags & b.collide_flags) == 0)
                continue;
            const Fixed ar = a.sep_radius.raw() > 0 ? a.sep_radius : a.radius;
            const Fixed br = b.sep_radius.raw() > 0 ? b.sep_radius : b.radius;
            const Fixed rsum = ar + br;
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
                if (a.moved_this_tick != b.moved_this_tick) {
                    if (a.moved_this_tick) {
                        pi = Fixed();
                        pj = pen / Fixed::from_int(2);
                    } else {
                        pi = pen / Fixed::from_int(2);
                        pj = Fixed();
                    }
                } else {
                    const bool allied = are_allied(a.player, b.player);
                    int32_t ra, rb;
                    if (allied && a.allied_push_priority != b.allied_push_priority) {
                        ra = a.allied_push_priority;
                        rb = b.allied_push_priority;
                    } else {
                        ra = a.push_priority;
                        rb = b.push_priority;
                    }
                    if (ra > rb) {
                        pi = Fixed();
                        pj = pen / Fixed::from_int(2);
                    } else if (rb > ra) {
                        pi = pen / Fixed::from_int(2);
                        pj = Fixed();
                    } else {
                        pi = pen / Fixed::from_int(4);
                        pj = pi;
                    }
                }
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
    for (Unit& u : units_) u.moved_this_tick = false;
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
            if (actor < units_.size() && units_[actor].is_worker) {
                Order o;
                o.type = OrderType::Gather;
                o.target = target;
                return o;
            }
            Order o;
            o.type = OrderType::Move;
            o.tx = t.x;
            o.ty = t.y;
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
    if (reserve_supply_)
        for (const Pending& pd : pending_)
            if (pd.proto.player == p)
                s = s + pd.proto.supply_uses * Fixed::from_int(pd.count > 0 ? pd.count : 1);
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
    EffectNode n;
    n.kind = def.kind;
    n.amount = def.amount;
    n.duration = def.duration;
    n.energy_fraction = def.energy_fraction;
    n.radius = def.radius;
    n.period = def.period;
    n.move_speed_mult = def.move_speed_mult;
    n.attack_speed_mult = def.attack_speed_mult;
    n.beh_root = def.beh_root;
    n.beh_stun = def.beh_stun;
    n.beh_lift = def.beh_lift;
    n.beh_control_transfer = def.beh_control_transfer;
    n.beh_dmg_per_tick = def.beh_dmg_per_tick;
    n.beh_dmg_period = def.beh_dmg_period;
    n.period_count = def.period_count;
    n.spawn_proto = def.spawn_proto;
    n.spawn_count = def.spawn_count;
    n.spawn_snap_resource = def.spawn_snap_resource;
    n.mineral_cost = def.mineral_cost;
    n.gas_cost = def.gas_cost;
    n.morph_heal_life_frac = def.morph_heal_life_frac;
    n.morph_heal_shield_frac = def.morph_heal_shield_frac;
    def.root = add_effect(std::move(n));
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
Fixed World::behavior_weapon_bonus(const Unit& u, std::uint32_t target_attrs) {
    Fixed bonus;
    for (const BehaviorInstance& b : u.behaviors)
        if (b.wpn_bonus_attr != 0 && (target_attrs & b.wpn_bonus_attr) != 0)
            bonus = bonus + b.wpn_bonus_amount;
    return bonus;
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

    EffectContext ctx;
    ctx.source = u.id;
    ctx.origin = u.player;
    ctx.sx = u.x;
    ctx.sy = u.y;
    ctx.tx = o.tx;
    ctx.ty = o.ty;
    ctx.target = o.target;
    ctx.range = ab.range;

    const bool has_root = ab.root >= 0 && ab.root < EffectRef(effects_.size());
    const EffectKind kind = has_root ? effects_[ab.root].kind : ab.kind;

    if (has_root ? root_is_point_cast(ab.root) : is_point_self(kind)) {
        u.orders.erase(u.orders.begin());
        execute_effect(ab.root, ctx);
        ++u.cast_count;
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

    u.orders.erase(u.orders.begin());
    execute_effect(ab.root, ctx);
    ++u.cast_count;
}

bool World::teleport_unit(uint32_t id, Fixed tx, Fixed ty) {
    if (id >= units_.size()) return false;
    Unit& u = units_[id];
    if (!u.alive) return false;
    u.x = tx;
    u.y = ty;
    u.path.clear();
    u.path_next = 0;
    return true;
}

void World::execute_effect(EffectRef ref, const EffectContext& ctx, int depth) {
    if (depth > kMaxEffectDepth) return;
    if (ref < 0 || ref >= EffectRef(effects_.size())) return;
    const EffectNode& n = effects_[ref];
    if (!n.gates.empty() && ctx.target < units_.size()) {
        const Unit& tgt = units_[ctx.target];
        for (const VitalGate& g : n.gates)
            if (!vital_gate_passes(g, tgt)) return;
    }
    switch (n.kind) {
        case EffectKind::Set:
            for (EffectRef c : n.children) execute_effect(c, ctx, depth + 1);
            return;
        case EffectKind::EnumArea: {
            if (n.children.empty()) return;
            const EffectRef child = n.children[0];
            const Fixed cx = ctx.tx, cy = ctx.ty, rad = n.radius;
            const std::uint8_t affil = n.search_affiliation;
            const int32_t caster_team = ctx.origin >= 0 ? team_of(ctx.origin) : -1;
            const bool skip_structures = n.search_exclude_structure;
            std::vector<uint32_t> ids;
            for (const Unit& u : units_) {
                if (!u.alive) continue;
                if (skip_structures && (u.attributes & kAttrStructure)) continue;
                std::uint8_t rel;
                if (u.id == ctx.source) rel = kAffilSelf;
                else if (is_neutral(u.player)) rel = kAffilNeutral;
                else if (u.player == ctx.origin) rel = kAffilPlayer;
                else if (caster_team >= 0 && team_of(u.player) == caster_team) rel = kAffilAlly;
                else rel = kAffilEnemy;
                if (!(affil & rel)) continue;
                const Fixed dx = u.x - cx, dy = u.y - cy, reach = rad + u.radius;
                if (dx * dx + dy * dy <= reach * reach) ids.push_back(u.id);
            }
            int32_t ridx = 0;
            for (uint32_t id : ids) {
                EffectContext cc = ctx;
                cc.target = id;
                cc.recall_index = ridx++;
                execute_effect(child, cc, depth + 1);
            }
            return;
        }
        case EffectKind::Switch: {
            const size_t ncase = n.switch_case_gates.size();
            for (size_t i = 0; i < ncase && i < n.children.size(); ++i) {
                const bool pass = ctx.target < units_.size() &&
                                  vital_gate_passes(n.switch_case_gates[i], units_[ctx.target]);
                if (pass) { execute_effect(n.children[i], ctx, depth + 1); return; }
            }
            if (n.children.size() > ncase) execute_effect(n.children[ncase], ctx, depth + 1);
            return;
        }
        case EffectKind::IssueOrder: {
            const uint32_t who = n.beh_on_caster ? ctx.source : ctx.target;
            if (who >= units_.size() || !units_[who].alive) return;
            if (n.issue_order == kIssueOrderStop) { stop(who); return; }
            Order o{};
            o.type = OrderType(n.issue_order);
            o.tx = ctx.tx;
            o.ty = ctx.ty;
            if (o.type == OrderType::Attack || o.type == OrderType::Gather) o.target = ctx.target;
            issue(who, o);
            return;
        }
        case EffectKind::CreateHealer:
            start_heal(ctx.source, ctx.target, n.amount, n.heal_drain_factor, ctx.range);
            return;
        case EffectKind::Teleport: {
            if (n.teleport_to_source) {
                static const int DX[8] = {1, 1, 0, -1, -1, -1, 0, 1};
                static const int DY[8] = {0, 1, 1, 1, 0, -1, -1, -1};
                if (ctx.recall_index >= 0) {
                    const int k = ctx.recall_index, ring = k / 8 + 1;
                    teleport_unit(ctx.target, ctx.sx + Fixed::from_int(DX[k & 7] * ring),
                                  ctx.sy + Fixed::from_int(DY[k & 7] * ring));
                } else {
                    teleport_unit(ctx.target, ctx.sx, ctx.sy);
                }
                return;
            }
            const Fixed dx = ctx.tx - ctx.sx, dy = ctx.ty - ctx.sy;
            if (ctx.range.raw() <= 0 || dx * dx + dy * dy <= ctx.range * ctx.range)
                teleport_unit(ctx.source, ctx.tx, ctx.ty);
            return;
        }
        case EffectKind::CreatePersistent: {
            PersistentInstance p;
            p.x = ctx.tx;
            p.y = ctx.ty;
            p.radius = n.radius;
            p.period = n.period;
            p.timer = n.period;
            p.amount = n.amount;
            p.count = n.period_count;
            p.owner = ctx.origin;
            p.periodic = n.periodic;
            add_persistent(p);
            return;
        }
        case EffectKind::CreateUnit: {
            if (n.spawn_proto >= 0 && n.spawn_proto < int32_t(spawn_protos_.size()) &&
                n.spawn_count > 0) {
                PendingSpawn ps{n.spawn_proto, n.spawn_count, ctx.tx, ctx.ty, ctx.origin};
                if (n.spawn_snap_resource) {
                    const uint32_t node = nearest_mineral_node(ctx.tx, ctx.ty);
                    if (node != kNoTarget) {
                        ps.x = units_[node].x;
                        ps.y = units_[node].y;
                        ps.gather_node = node;
                        ps.depot = ctx.source;
                    }
                }
                pending_spawns_.push_back(ps);
            }
            return;
        }
        case EffectKind::Reveal:
            drop_scan(ctx.tx, ctx.ty, n.radius, n.duration);
            return;
        case EffectKind::Force: {
            if (ctx.target >= units_.size() || !units_[ctx.target].alive) return;
            const Unit& t = units_[ctx.target];
            const Fixed dx = t.x - ctx.sx, dy = t.y - ctx.sy;
            apply_knockback_to(ctx.target, ctx.sx, ctx.sy, isqrt(dx * dx + dy * dy), n.duration);
            return;
        }
        case EffectKind::PhaseShift: {
            if (ctx.source >= units_.size() || !units_[ctx.source].alive) return;
            if (n.spawn_proto < 0 || n.spawn_proto >= int32_t(spawn_protos_.size())) return;
            if (n.duration.raw() <= 0) return;
            if (units_[ctx.source].phase_timer.raw() > 0) return;
            PendingSpawn ps{n.spawn_proto, 1, ctx.sx, ctx.sy, ctx.origin};
            ps.phase_link = ctx.source;
            ps.move_x = ctx.tx;
            ps.move_y = ctx.ty;
            pending_spawns_.push_back(ps);
            units_[ctx.source].phase_timer = n.duration;
            return;
        }
        case EffectKind::Inject: {
            if (ctx.source >= units_.size() || !units_[ctx.source].alive) return;
            if (ctx.target >= units_.size() || !units_[ctx.target].alive) return;
            Unit& hall = units_[ctx.target];
            if (hall.spawner_proto < 0 || n.spawn_count <= 0 || n.duration.raw() <= 0) return;
            hall.inject_proto = hall.spawner_proto;
            hall.inject_count = n.spawn_count;
            hall.inject_timer = n.duration;
            if (n.period.raw() > 0) {
                BehaviorInstance b;
                b.kind = kCastBehaviorKind;
                b.timer = n.period;
                b.stun = true;
                add_behavior(ctx.source, b);
            }
            return;
        }
        case EffectKind::Morph: {
            if (ctx.source >= units_.size()) return;
            if (n.spawn_proto >= 0 && n.spawn_proto < int32_t(spawn_protos_.size())) {
                Player& pl = player(ctx.origin);
                if (pl.minerals >= n.mineral_cost && pl.gas >= n.gas_cost) {
                    pl.minerals -= n.mineral_cost;
                    pl.gas -= n.gas_cost;
                    Unit& u = units_[ctx.source];
                    u.morph_proto = n.spawn_proto;
                    u.morph_timer = n.duration;
                    u.morph_heal_life_frac = n.morph_heal_life_frac;
                    u.morph_heal_shield_frac = n.morph_heal_shield_frac;
                }
            }
            return;
        }
        case EffectKind::Damage:
            if (n.shield_bonus.raw() > 0 && ctx.target < units_.size()) {
                Unit& st = units_[ctx.target];
                if (st.alive && st.shields.raw() > 0) {
                    const Fixed strip = st.shields < n.shield_bonus ? st.shields : n.shield_bonus;
                    st.shields = st.shields - strip;
                }
            }
            apply_spell_damage(ctx.target, n.amount, ctx.source, n.armor_reduction);
            return;
        case EffectKind::DrainEnergy: {
            if (ctx.target >= units_.size()) return;
            Unit& et = units_[ctx.target];
            const Fixed drain = et.energy < n.amount ? et.energy : n.amount;
            et.energy = et.energy - drain;
            return;
        }
        case EffectKind::WeaponDamage: {
            const Fixed amt = ctx.has_weapon ? ctx.wpn_amount : n.amount;
            const std::uint32_t battr = ctx.has_weapon ? ctx.wpn_bonus_attr : n.wpn_bonus_attr;
            const Fixed bamt = ctx.has_weapon ? ctx.wpn_bonus_amount : n.wpn_bonus_amount;
            if (amt.raw() <= 0) return;
            apply_weapon_hit(ctx.target, amt, battr, bamt, ctx.source);
            return;
        }
        case EffectKind::Heal:
            heal_unit(ctx.target, n.amount);
            return;
        case EffectKind::Buff: {
            if (ctx.target >= units_.size()) return;
            Unit& tgt = units_[ctx.target];
            if (tgt.buff_timer.raw() <= 0) {
                tgt.wpn_damage = tgt.wpn_damage + n.amount;
                tgt.buff_damage = n.amount;
            }
            tgt.buff_timer = n.duration;
            return;
        }
        case EffectKind::ApplyBehavior: {
            BehaviorInstance b;
            b.kind = n.beh_control_transfer ? kCastBehaviorKindControl : kCastBehaviorKind;
            b.src = n.behavior_tag;
            b.timer = n.duration.raw() > 0 ? n.duration : Fixed::from_raw(1);
            b.speed_mult = n.move_speed_mult;
            b.root = n.beh_root;
            b.stun = n.beh_stun;
            b.lift = n.beh_lift;
            b.dmg_per_tick = n.beh_dmg_per_tick;
            b.dmg_period = n.beh_dmg_period;
            b.dmg_timer = n.beh_dmg_period;
            if (n.beh_control_transfer && ctx.target < units_.size() && ctx.source < units_.size()) {
                Unit& ctgt = units_[ctx.target];
                int32_t orig = ctgt.player;
                for (const BehaviorInstance& e : ctgt.behaviors)
                    if (e.ctrl_orig_owner != -2) { orig = e.ctrl_orig_owner; break; }
                b.ctrl_orig_owner = orig;
                b.ctrl_caster = ctx.source;
                ctgt.player = units_[ctx.source].player;
            }
            add_behavior(ctx.target, b);
            if (n.beh_lift && ctx.target < units_.size() && units_[ctx.target].alive &&
                !units_[ctx.target].flying) {
                units_[ctx.target].flying = true;
                units_[ctx.target].orders.clear();
            }
            if (ctx.target < units_.size() &&
                n.attack_speed_mult.raw() != Fixed::from_int(1).raw())
                units_[ctx.target].atk_speed_mult = n.attack_speed_mult;
            return;
        }
        case EffectKind::RemoveBehavior: {
            const uint32_t who = n.beh_on_caster ? ctx.source : ctx.target;
            if (who >= units_.size()) return;
            auto& bs = units_[who].behaviors;
            bs.erase(std::remove_if(bs.begin(), bs.end(),
                                    [&](const BehaviorInstance& bi) { return bi.src == n.behavior_tag; }),
                     bs.end());
            return;
        }
        case EffectKind::ModifyEnergy: {
            if (ctx.target >= units_.size()) return;
            Unit& tgt = units_[ctx.target];
            const Fixed dmg = tgt.energy * n.energy_fraction;
            tgt.energy = Fixed();
            apply_spell_damage(ctx.target, dmg, ctx.source, n.armor_reduction);
            return;
        }
    }
}

void World::apply_spell_damage(uint32_t target, Fixed dmg, uint32_t caster, Fixed armor_reduction) {
    if (target >= units_.size()) return;
    Unit& t = units_[target];
    if (!t.alive) return;
    const Fixed vitals_before = t.hp + t.shields;
    const bool barrier_was_up = t.barrier.raw() > 0;
    (void)armor_reduction;
    dmg = dmg * t.dmg_fraction;
    if (dmg.raw() > 0 && dmg < min_damage()) dmg = min_damage();
    if (t.barrier.raw() > 0 && dmg.raw() > 0) {
        if (dmg <= t.barrier) {
            t.barrier = t.barrier - dmg;
            dmg = Fixed();
        } else {
            dmg = dmg - t.barrier;
            t.barrier = Fixed();
        }
    }
    if (t.shields.raw() > 0) {
        if (t.shields >= dmg) {
            t.shields = t.shields - dmg;
            dmg = Fixed();
        } else {
            dmg = dmg - t.shields;
            t.shields = Fixed();
        }
    }
    if (dmg.raw() > 0) t.hp = t.hp - dmg;
    if (t.hp.raw() > 0 && t.barrier_max.raw() > 0 && t.barrier_cd.raw() == 0 && !barrier_was_up &&
        (t.hp + t.shields) < vitals_before) {
        t.barrier = t.barrier_max;
        t.barrier_cd = t.barrier_cd_max;
    }
    if (t.hp.raw() <= 0 && t.alive) {
        t.alive = false;
        if (caster < units_.size() && units_[caster].alive) award_kill_xp(units_[caster], t.kill_xp);
        emit_death_reveal(t, caster);
    }
    record_flee_trigger(t, caster, vitals_before);
}

void World::apply_weapon_hit(uint32_t target, Fixed base, uint32_t bonus_attr, Fixed bonus_amount,
                             uint32_t attacker, Fixed armor_reduction) {
    if (target >= units_.size()) return;
    Unit& t = units_[target];
    if (!t.alive) return;
    Defender d{t.hp, t.shields, t.armor, t.damage_reduction + t.aura_reduction};
    d.shield_armor = t.shield_armor;
    d.attributes = t.attributes;
    d.dmg_fraction = t.dmg_fraction;
    d.dmg_clamp_max = t.dmg_clamp_max;
    d.dmg_minimum = t.dmg_minimum;
    d.barrier = t.barrier;
    const bool barrier_was_up = t.barrier.raw() > 0;
    const Fixed vitals_before = t.hp + t.shields;
    apply_hit(Weapon{base, Fixed(), bonus_attr, bonus_amount, armor_reduction}, d);
    t.hp = d.life;
    t.shields = d.shields;
    t.barrier = d.barrier;
    if (t.hp.raw() > 0 && t.barrier_max.raw() > 0 && t.barrier_cd.raw() == 0 && !barrier_was_up &&
        (t.hp + t.shields) < vitals_before) {
        t.barrier = t.barrier_max;
        t.barrier_cd = t.barrier_cd_max;
    }
    if (t.hp.raw() <= 0 && t.alive) {
        t.alive = false;
        if (attacker < units_.size() && units_[attacker].alive) award_kill_xp(units_[attacker], t.kill_xp);
        emit_death_reveal(t, attacker);
    }
    record_flee_trigger(t, attacker, vitals_before);
}

void World::record_flee_trigger(Unit& t, uint32_t attacker, Fixed vitals_before) {
    if (t.alive && t.flees_when_attacked && (t.hp + t.shields) < vitals_before &&
        attacker < units_.size() && units_[attacker].alive && units_[attacker].player >= 0 &&
        units_[attacker].player != t.player)
        t.flee_from = attacker;
}

void World::apply_weapon_fire(uint32_t attacker, uint32_t target, Fixed w_dmg, uint32_t w_battr,
                              Fixed w_bamt, int hits, Fixed w_ar) {
    Unit& u = units_[attacker];
    Unit& tgt = units_[target];
    const uint32_t atk = target;
    const Fixed primary_dmg =
        u.behaviors.empty() ? w_dmg : w_dmg + behavior_weapon_bonus(u, tgt.attributes);
    if (u.wpn_effect >= 0) {
        const int n = (multi_hit_armor_ && hits > 1) ? hits : 1;
        const Fixed per = primary_dmg / Fixed::from_int(n);
        const Fixed bper = w_bamt / Fixed::from_int(n);
        Fixed dacc, bacc;
        for (int k = 0; k < n; ++k) {
            const bool last = (k == n - 1);
            EffectContext ctx;
            ctx.source = u.id;
            ctx.target = tgt.id;
            ctx.origin = u.player;
            ctx.has_weapon = true;
            ctx.wpn_amount = last ? (primary_dmg - dacc) : per;
            ctx.wpn_bonus_attr = w_battr;
            ctx.wpn_bonus_amount = last ? (w_bamt - bacc) : bper;
            dacc = dacc + per;
            bacc = bacc + bper;
            execute_effect(u.wpn_effect, ctx);
        }
    } else if (multi_hit_armor_ && hits > 1) {
        const Fixed per = primary_dmg / Fixed::from_int(hits);
        const Fixed bper = w_bamt / Fixed::from_int(hits);
        Fixed dacc, bacc;
        for (int k = 0; k < hits; ++k) {
            const bool last = (k == hits - 1);
            const Fixed h = last ? (primary_dmg - dacc) : per;
            const Fixed bh = last ? (w_bamt - bacc) : bper;
            dacc = dacc + per;
            bacc = bacc + bper;
            apply_weapon_hit(tgt.id, h, w_battr, bh, u.id, w_ar);
        }
    } else {
        apply_weapon_hit(tgt.id, primary_dmg, w_battr, w_bamt, u.id, w_ar);
    }
    if (u.attack_spawn_proto >= 0 && u.attack_spawn_count > 0 &&
        u.attack_spawn_proto < int32_t(spawn_protos_.size()))
        pending_spawns_.push_back(
            {u.attack_spawn_proto, u.attack_spawn_count, tgt.x, tgt.y, u.player, kNoTarget, false});
    const bool suppress_splash = u.no_splash_radius.raw() > 0 && tgt.radius >= u.no_splash_radius;
    if (u.splash_outer.raw() > 0 && !suppress_splash) {
        const Fixed tx = tgt.x, ty = tgt.y;
        for (Unit& o : units_) {
            if (!o.alive || o.id == atk || are_allied(o.player, u.player)) continue;
            if (o.loaded || o.burrowed) continue;
            const Fixed dx = o.x - tx, dy = o.y - ty;
            const Fixed d2 = dx * dx + dy * dy;
            Fixed frac;
            if (d2 <= u.splash_inner * u.splash_inner) frac = u.splash_inner_frac;
            else if (d2 <= u.splash_mid * u.splash_mid) frac = u.splash_mid_frac;
            else if (d2 <= u.splash_outer * u.splash_outer) frac = u.splash_outer_frac;
            else continue;
            apply_weapon_hit(o.id, w_dmg * frac, w_battr, w_bamt * frac, u.id, w_ar);
        }
    }
    if (u.splash_line_count > 0) {
        const Fixed tx = tgt.x, ty = tgt.y;
        for (Unit& o : units_) {
            if (!o.alive || o.id == atk || are_allied(o.player, u.player)) continue;
            if (o.loaded || o.burrowed || o.flying) continue;
            if (in_line_splash(u.x, u.y, tx, ty, u.splash_line_first, u.splash_line_step,
                               u.splash_line_count, u.splash_line_radius, o.x, o.y,
                               u.splash_line_from_source))
                apply_weapon_hit(o.id, w_dmg, w_battr, w_bamt, u.id, w_ar);
        }
    }
    if (u.arc_radius[0].raw() > 0 || u.arc_radius[1].raw() > 0) {
        const Fixed tx = tgt.x, ty = tgt.y;
        for (Unit& o : units_) {
            if (!o.alive || o.id == atk || are_allied(o.player, u.player)) continue;
            if (o.loaded || o.burrowed || o.flying) continue;
            Fixed frac;
            for (int k = 0; k < 2; ++k)
                if (u.arc_radius[k].raw() > 0 && u.arc_frac[k].raw() > 0 &&
                    in_arc(u.x, u.y, tx, ty, u.arc_radius[k], u.arc_cos[k], o.x, o.y))
                    frac = frac + u.arc_frac[k];
            if (frac.raw() > 0)
                apply_weapon_hit(o.id, w_dmg * frac, w_battr, w_bamt * frac, u.id, w_ar);
        }
    }
    if (u.bounce_count > 0 && u.bounce_range.raw() > 0) {
        const std::uint32_t n = std::uint32_t(units_.size());
        std::vector<std::uint8_t> hit(n, 0);
        if (atk < n) hit[atk] = 1;
        std::uint32_t from = atk;
        const std::int64_t r2 = std::int64_t(u.bounce_range.raw()) * std::int64_t(u.bounce_range.raw());
        const int nb = u.bounce_count < Unit::kMaxBounce ? u.bounce_count : Unit::kMaxBounce;
        for (int b = 0; b < nb; ++b) {
            const Unit& src = units_[from];
            const Fixed fx = src.x, fy = src.y;
            std::uint32_t best = kNoTarget;
            std::int64_t best_d2 = 0;
            for (std::uint32_t i = 0; i < n; ++i) {
                if (hit[i]) continue;
                const Unit& o = units_[i];
                if (!o.alive || are_allied(o.player, u.player)) continue;
                if (o.loaded || o.burrowed) continue;
                const std::uint32_t plane_bit = o.flying ? 2u : 1u;
                if ((u.wpn_target_filter & plane_bit) == 0) continue;
                const std::int64_t dx = std::int64_t((o.x - fx).raw()), dy = std::int64_t((o.y - fy).raw());
                const std::int64_t d2 = dx * dx + dy * dy;
                if (d2 > r2) continue;
                if (best == kNoTarget || d2 < best_d2) {
                    best = i;
                    best_d2 = d2;
                }
            }
            if (best == kNoTarget) break;
            apply_weapon_hit(best, u.bounce_amount[b], 0u, Fixed(), u.id, w_ar);
            hit[best] = 1;
            from = best;
        }
    }
}

void World::step_point_defense(Fixed dt) {
    (void)dt;
    if (!point_defense_enabled_ || pending_hits_.empty()) return;
    for (const Unit& d : units_) {
        if (!d.alive || d.point_defense_radius.raw() <= 0 || d.point_defense_max <= 0) continue;
        const std::int64_t r2 =
            std::int64_t(d.point_defense_radius.raw()) * std::int64_t(d.point_defense_radius.raw());
        std::vector<std::pair<std::int64_t, std::size_t>> cand;
        for (std::size_t i = 0; i < pending_hits_.size(); ++i) {
            PendingHit& p = pending_hits_[i];
            if (p.life.raw() <= 0) continue;
            if (p.attacker >= units_.size()) continue;
            if (are_allied(units_[p.attacker].player, d.player)) continue;
            const std::int64_t dx = std::int64_t((p.px - d.x).raw()), dy = std::int64_t((p.py - d.y).raw());
            const std::int64_t dd = dx * dx + dy * dy;
            if (dd > r2) continue;
            cand.emplace_back(dd, i);
        }
        std::stable_sort(cand.begin(), cand.end(),
                         [](const auto& a, const auto& b) { return a.first < b.first; });
        int killed = 0;
        for (const auto& c : cand) {
            if (killed >= d.point_defense_max) break;
            pending_hits_[c.second].life = Fixed();
            ++killed;
        }
    }
}

void World::step_windup(Fixed dt) {
    if (!attack_windup_) return;
    const std::uint32_t n = std::uint32_t(units_.size());
    for (std::uint32_t i = 0; i < n; ++i) {
        Unit& u = units_[i];
        if (u.windup_timer.raw() <= 0) continue;
        u.windup_timer = u.windup_timer - dt;
        if (u.windup_timer.raw() > 0) continue;
        u.windup_timer = Fixed();
        const std::uint32_t t = u.windup_target;
        u.windup_target = kNoTarget;
        const Fixed ps = u.windup_ps;
        u.windup_ps = Fixed();
        if (u.alive && t < units_.size() && units_[t].alive) {
            if (projectile_travel_ && ps.raw() > 0)
                queue_projectile(i, u.x, u.y, t, ps, u.windup_dmg, u.windup_battr, u.windup_bamt, u.windup_ar);
            else
                apply_weapon_fire(i, t, u.windup_dmg, u.windup_battr, u.windup_bamt, 1, u.windup_ar);
        }
    }
}

void World::queue_projectile(std::uint32_t attacker, Fixed px, Fixed py, std::uint32_t target, Fixed speed,
                             Fixed dmg, std::uint32_t battr, Fixed bamt, Fixed ar) {
    Fixed flife = Fixed::from_int(1);
    if (target < units_.size()) {
        const Fixed fdx = units_[target].x - px, fdy = units_[target].y - py;
        flife = isqrt(fdx * fdx + fdy * fdy) * Fixed::from_int(4) / speed + Fixed::from_int(1);
    }
    pending_hits_.push_back({attacker, target, px, py, speed, dmg, battr, bamt, flife, ar});
}

void World::step_projectiles(Fixed dt) {
    if (pending_hits_.empty()) return;
    std::size_t w = 0;
    for (std::size_t r = 0; r < pending_hits_.size(); ++r) {
        PendingHit p = pending_hits_[r];
        if (p.attacker >= units_.size() || p.target >= units_.size() || !units_[p.target].alive)
            continue;
        p.life = p.life - dt;
        if (p.life.raw() <= 0) continue;
        const Unit& t = units_[p.target];
        const Fixed step = p.speed * dt;
        const Fixed dx = t.x - p.px, dy = t.y - p.py;
        const Fixed d2 = dx * dx + dy * dy;
        if (d2 <= step * step) {
            apply_weapon_fire(p.attacker, p.target, p.dmg, p.battr, p.bamt, 1, p.ar);
            continue;
        }
        const Fixed d = isqrt(d2);
        if (d.raw() > 0) {
            p.px = p.px + dx * step / d;
            p.py = p.py + dy * step / d;
        }
        pending_hits_[w++] = p;
    }
    pending_hits_.resize(w);
}

Fixed World::heal_unit(uint32_t id, Fixed amount) {
    if (id >= units_.size() || amount.raw() <= 0) return Fixed();
    Unit& u = units_[id];
    if (!u.alive || u.hp >= u.hp_max) return Fixed();
    Fixed heal = amount;
    const Fixed missing = u.hp_max - u.hp;
    if (heal > missing) heal = missing;
    u.hp = u.hp + heal;
    return heal;
}

void World::emit_death_reveal(const Unit& victim, uint32_t killer) {
    if (!death_reveal_enabled_ || killer >= units_.size()) return;
    const int32_t p = units_[killer].player;
    if (p < 0) return;
    death_reveal_.drop(p, victim.x, victim.y, victim.death_reveal_radius,
                       Fixed::from_int(kDeathRevealDurationSec));
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
                        int32_t gas_cost, Fixed build_time, int32_t count) {
    if (producer >= units_.size() || units_[producer].constructing ||
        units_[producer].addon_building != 0)
        return false;
    if (!tech_requirements_met(units_[producer].player, proto.catalog_id)) return false;
    if (!techlab_gated_.empty() && techlab_gated_.count(proto.catalog_id) &&
        units_[producer].addon != kAddonTechLab)
        return false;
    if (const int32_t slots = units_[producer].train_slots; slots > 0) {
        int in_flight = 0;
        for (const Pending& p : pending_)
            if (p.producer == producer) ++in_flight;
        if (in_flight >= slots) return false;
    }
    uint32_t consumed_larva = kNoTarget;
    uint32_t egg_id = kNoTarget;
    if (units_[producer].spawner_proto >= 0 && proto.speed.raw() > 0) {
        uint32_t larva = kNoTarget;
        for (uint32_t i = 0; i < units_.size(); ++i)
            if (units_[i].alive && units_[i].spawn_parent == producer) { larva = i; break; }
        if (larva == kNoTarget) return false;
        if (larva_egg_morph_ && egg_proto_ >= 0) {
            const Fixed lx = units_[larva].x, ly = units_[larva].y;
            Unit egg = spawn_protos_[egg_proto_];
            egg.id = units_[larva].id;
            egg.player = units_[larva].player;
            egg.x = lx;
            egg.y = ly;
            egg.spawn_parent = kNoTarget;
            units_[larva] = egg;
            egg_id = larva;
        } else {
            units_[larva].alive = false;
            consumed_larva = larva;
        }
    }
    int32_t pl = units_[producer].player;
    Player& pp = player(pl);
    const int32_t n = count > 1 ? count : 1;
    pp.minerals -= mineral_cost * n;
    pp.gas -= gas_cost * n;
    Unit u = proto;
    u.player = pl;
    Pending pd{producer, u, build_time};
    pd.count = n;
    pd.mineral_cost = mineral_cost * n;
    pd.gas_cost = gas_cost * n;
    pd.consumed_larva = consumed_larva;
    if (egg_id != kNoTarget) {
        pd.egg = egg_id;
        pd.has_pos = true;
        pd.px = units_[egg_id].x;
        pd.py = units_[egg_id].y;
    }
    pending_.push_back(std::move(pd));
    return true;
}

bool World::cancel_train(uint32_t producer) {
    int idx = -1;
    for (int i = int(pending_.size()) - 1; i >= 0; --i)
        if (pending_[i].producer == producer) { idx = i; break; }
    if (idx < 0) return false;
    const Pending& pd = pending_[idx];
    Player& pp = player(pd.proto.player);
    pp.minerals += pd.mineral_cost;
    pp.gas += pd.gas_cost;
    if (pd.egg != kNoTarget) {
        if (pd.egg < units_.size()) units_[pd.egg].alive = false;
    } else if (pd.consumed_larva < units_.size() && !units_[pd.consumed_larva].alive) {
        units_[pd.consumed_larva].alive = true;
    }
    pending_.erase(pending_.begin() + idx);
    return true;
}

bool World::cancel_construction(uint32_t building, int32_t mineral_cost, int32_t gas_cost) {
    if (building >= units_.size()) return false;
    Unit& u = units_[building];
    if (!u.alive || !u.constructing) return false;
    Player& pp = player(u.player);
    pp.minerals += (mineral_cost * 3) / 4;
    pp.gas += (gas_cost * 3) / 4;
    const Fixed bx = u.x, by = u.y;
    const int32_t bplayer = u.player;
    u.alive = false;
    u.constructing = false;
    auto mf = morph_founder_.find(building);
    if (mf != morph_founder_.end()) {
        Unit drone = mf->second;
        morph_founder_.erase(mf);
        drone.player = bplayer;
        drone.x = bx;
        drone.y = by;
        drone.alive = true;
        drone.constructing = false;
        drone.hp = drone.hp_max;
        drone.shields = drone.shields_max;
        drone.cooldown = Fixed();
        drone.target = kNoTarget;
        drone.construct_target = kNoTarget;
        drone.orders.clear();
        add_unit(drone);
    }
    return true;
}

bool World::cancel_worker_build(uint32_t builder, int32_t mineral_cost, int32_t gas_cost) {
    for (size_t i = 0; i < worker_builds_.size(); ++i) {
        if (worker_builds_[i].builder != builder) continue;
        if (builder < units_.size()) {
            Player& pp = player(units_[builder].player);
            pp.minerals += mineral_cost;
            pp.gas += gas_cost;
            units_[builder].orders.clear();
        }
        worker_builds_.erase(worker_builds_.begin() + i);
        return true;
    }
    return false;
}

void World::set_tech_requirement(std::string unit_id, std::vector<std::string> prereqs) {
    tech_reqs_[std::move(unit_id)] = std::move(prereqs);
}

bool World::tech_requirements_met(int32_t player, const std::string& unit_id) const {
    auto it = tech_reqs_.find(unit_id);
    if (it == tech_reqs_.end()) return true;
    for (const std::string& need : it->second) {
        bool have = false;
        for (const Unit& u : units_)
            if (u.alive && !u.constructing && u.player == player && u.catalog_id == need) { have = true; break; }
        if (!have) return false;
    }
    return true;
}

bool World::issue_worker_build(uint32_t builder, Fixed x, Fixed y, const Unit& proto,
                               int32_t mineral_cost, int32_t gas_cost, Fixed build_time,
                               Fixed arrive, BuildVerb verb) {
    if (builder >= units_.size() || !units_[builder].alive) return false;
    if (proto.requires_creep && !creep_.is_creep(x, y)) return false;
    if (proto.needs_power && !powered_at(x, y, units_[builder].player)) return false;
    if (proto.requires_vespene) {
        bool on_node = false;
        for (const Unit& n : units_) {
            if (!n.alive || !n.is_gas || n.resource_left <= 0) continue;
            const Fixed dx = n.x - x, dy = n.y - y;
            const Fixed rr = n.radius.raw() > 0 ? n.radius : Fixed::from_int(1);
            if (dx * dx + dy * dy <= rr * rr) {
                on_node = true;
                break;
            }
        }
        if (!on_node) return false;
    }
    Player& pp = player(units_[builder].player);
    pp.minerals -= mineral_cost;
    pp.gas -= gas_cost;
    Order o;
    o.type = OrderType::Move;
    o.tx = x;
    o.ty = y;
    issue(builder, o, true);
    worker_builds_.push_back(WorkerBuild{builder, x, y, proto, build_time, arrive, verb});
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
        u.power_managed = true;
        const uint32_t id = add_unit(u);
        Fixed start_hp = Fixed::from_raw(u.hp_max.raw() / 10);
        if (start_hp.raw() <= 0) start_hp = Fixed::from_int(1);
        Fixed bt = wb.build_time.raw() > 0 ? wb.build_time : Fixed::from_int(1);
        start_construction(id, bt, start_hp);
        if (wb.verb == BuildVerb::Warp) {
            units_[id].needs_builder = false;
            Unit& w = units_[wb.builder];
            w.construct_target = kNoTarget;
            w.orders.clear();
            if (w.is_worker)
                issue(wb.builder, Order{OrderType::Gather, Fixed(), Fixed(), kNoTarget}, false);
        } else if (wb.verb == BuildVerb::Morph) {
            units_[id].needs_builder = false;
            Unit& drone = units_[wb.builder];
            drone.construct_target = kNoTarget;
            drone.orders.clear();
            morph_founder_[id] = drone;
            drone.alive = false;
        } else {
            units_[id].needs_builder = true;
            units_[id].builder = wb.builder;
            units_[wb.builder].construct_target = id;
            units_[wb.builder].orders.clear();
        }
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
                           int32_t gas_cost, Fixed build_time, Fixed x, Fixed y, int32_t count) {
    if (producer >= units_.size() || units_[producer].constructing) return false;
    if (!tech_requirements_met(units_[producer].player, proto.catalog_id)) return false;
    if (const int32_t slots = units_[producer].train_slots; slots > 0) {
        int in_flight = 0;
        for (const Pending& p : pending_)
            if (p.producer == producer) ++in_flight;
        if (in_flight >= slots) return false;
    }
    int32_t pl = units_[producer].player;
    Player& pp = player(pl);
    const int32_t n = count > 1 ? count : 1;
    pp.minerals -= mineral_cost * n;
    pp.gas -= gas_cost * n;
    Unit u = proto;
    u.player = pl;
    Pending pd{producer, u, build_time};
    pd.has_pos = true;
    pd.px = x;
    pd.py = y;
    pd.count = n;
    pd.mineral_cost = mineral_cost * n;
    pd.gas_cost = gas_cost * n;
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
            apply_weapon_hit(best, p.wpn_damage, p.wpn_bonus_attr, p.wpn_bonus_amount, uint32_t(pi));
            p.cooldown = p.wpn_period;
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
            Fixed cx = p.x, cy = p.y;
            if (!p.offsets.empty()) {
                const size_t k = size_t(p.applied) < p.offsets.size() ? size_t(p.applied)
                                                                      : p.offsets.size() - 1;
                const Fixed ox = p.offsets[k].first, oy = p.offsets[k].second;
                cx = p.x + ox * p.face_x - oy * p.face_y;
                cy = p.y + ox * p.face_y + oy * p.face_x;
            }
            for (Unit& u : units_) {
                if (!u.alive || u.player == p.owner) continue;
                const Fixed dx = u.x - cx, dy = u.y - cy;
                const Fixed reach = p.radius + u.radius;
                if (dx * dx + dy * dy > reach * reach) continue;
                if (p.periodic >= 0) {
                    EffectContext ctx;
                    ctx.source = kNoTarget;
                    ctx.target = u.id;
                    ctx.origin = p.owner;
                    execute_effect(p.periodic, ctx);
                } else if (p.amount.raw() > 0) {
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
            p.applied += 1;
        }
        ++i;
    }
}

bool World::start_heal(std::uint32_t caster, std::uint32_t target, Fixed heal_rate, Fixed drain_factor,
                       Fixed range) {
    if (caster >= units_.size() || target >= units_.size() || caster == target) return false;
    const Unit& c = units_[caster];
    const Unit& t = units_[target];
    if (!c.alive || !t.alive || t.hp >= t.hp_max) return false;
    for (HealerInstance& h : healers_)
        if (h.caster == caster && h.target == target) {
            h.heal_rate = heal_rate;
            h.drain_factor = drain_factor;
            h.range = range;
            return true;
        }
    healers_.push_back(HealerInstance{caster, target, heal_rate, drain_factor, range});
    return true;
}

void World::step_healers(Fixed dt) {
    for (size_t i = 0; i < healers_.size();) {
        HealerInstance& h = healers_[i];
        bool retire = h.caster >= units_.size() || h.target >= units_.size();
        if (!retire) {
            Unit& c = units_[h.caster];
            Unit& t = units_[h.target];
            if (!c.alive || !t.alive || t.hp >= t.hp_max)
                retire = true;
            else {
                const Fixed dx = t.x - c.x, dy = t.y - c.y;
                const Fixed reach = h.range + c.radius + t.radius;
                if (h.range.raw() > 0 && dx * dx + dy * dy > reach * reach) {
                    retire = true;
                } else {
                    const Fixed heal = h.heal_rate * dt;
                    const Fixed cost = heal * h.drain_factor;
                    if (c.energy_max.raw() > 0 && c.energy < cost) {
                        retire = true;
                    } else {
                        if (c.energy_max.raw() > 0) c.energy = c.energy - cost;
                        Fixed nh = t.hp + heal;
                        if (nh > t.hp_max) nh = t.hp_max;
                        t.hp = nh;
                    }
                }
            }
        }
        if (retire)
            healers_.erase(healers_.begin() + i);
        else
            ++i;
    }
}

void World::step_production(Fixed dt) {
    for (size_t i = 0; i < pending_.size();) {
        Pending& pd = pending_[i];
        if (pd.egg != kNoTarget && (pd.egg >= units_.size() || !units_[pd.egg].alive)) {
            pending_.erase(pending_.begin() + i);
            continue;
        }
        if (pd.producer < units_.size()) {
            const Unit& pr = units_[pd.producer];
            if (pr.needs_power && pr.power_managed && !pr.powered) { ++i; continue; }
        }
        Fixed adv = dt;
        if (pd.producer < units_.size() && units_[pd.producer].chrono_timer.raw() > 0)
            adv = adv + adv / Fixed::from_int(2);
        pd.timer = pd.timer - adv;
        if (pd.timer.raw() <= 0) {
            if (pd.egg != kNoTarget && pd.egg < units_.size()) units_[pd.egg].alive = false;
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
            for (int32_t k = 1; k < pd.count; ++k) {
                static const int DXk[8] = {1, 1, 0, -1, -1, -1, 0, 1};
                static const int DYk[8] = {0, 1, 1, 1, 0, -1, -1, -1};
                Unit extra = pd.proto;
                extra.player = units_[nid].player;
                extra.x = units_[nid].x + Fixed::from_int(DXk[k & 7]);
                extra.y = units_[nid].y + Fixed::from_int(DYk[k & 7]);
                const uint32_t enid = add_unit(extra);
                if (rally) issue(enid, Order{OrderType::Move, rx, ry, kNoTarget}, false);
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
                int32_t amt = (u.carry_override && u.carry_amount > 0)
                                  ? u.carry_amount
                                  : (m.yield_amount > 0 ? m.yield_amount
                                                        : (u.carry_amount > 0 ? u.carry_amount : 5));
                if (m.resource_left < amt) amt = m.resource_left;
                u.carrying = amt;
                u.carry_source = u.gather_target;
                m.resource_left -= amt;
                if (m.resource_left <= 0 && !m.is_gas) m.alive = false;
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
            if (m.miners_max > 0 && !u.saturation_exempt) {
                int active = 0;
                for (const Unit& w : units_)
                    if (w.alive && w.id != u.id && !w.saturation_exempt &&
                        w.gather_target == u.gather_target && w.mine_cd.raw() > 0)
                        ++active;
                if (active >= m.miners_max) return;
            }
            u.mine_cd = m.harvest_time.raw() > 0
                            ? m.harvest_time
                            : (u.mine_time.raw() > 0 ? u.mine_time
                                                     : Fixed::parse(m.is_gas ? "1.981" : "2.786"));
            if (u.mine_time_mult.raw() > 0) u.mine_cd = u.mine_cd * u.mine_time_mult;
        } else {
            move_unit_toward(u, m.x, m.y, move_step(u, dt));
        }
    }
}

void World::step_interceptors(Fixed dt) {
    if (carriers_.empty()) return;
    for (Carrier& c : carriers_) {
        if (c.carrier >= units_.size()) continue;
        if (!units_[c.carrier].alive) {
            for (uint32_t id : c.wing)
                if (id < units_.size()) units_[id].alive = false;
            continue;
        }
        if (c.count < c.count_max && c.build_time.raw() > 0) {
            c.build_timer = c.build_timer - dt;
            if (c.build_timer.raw() <= 0) {
                ++c.count;
                c.build_timer = c.build_time;
            }
        }
        const Unit host = units_[c.carrier];

        uint32_t target = kNoTarget;
        if (!host.orders.empty() && host.orders.front().type == OrderType::Attack) {
            const uint32_t tgt = host.orders.front().target;
            if (tgt < units_.size() && units_[tgt].alive) {
                const Fixed dx = units_[tgt].x - host.x, dy = units_[tgt].y - host.y;
                if (c.leash.raw() <= 0 || dx * dx + dy * dy <= c.leash * c.leash) target = tgt;
            }
        }

        if (target != kNoTarget) {
            while (int32_t(c.wing.size()) < c.count && c.proto_idx >= 0 &&
                   c.proto_idx < int32_t(spawn_protos_.size())) {
                Unit ic = spawn_protos_[c.proto_idx];
                ic.player = host.player;
                ic.x = host.x;
                ic.y = host.y;
                ic.loaded = true;
                ic.alive = true;
                ic.orders.clear();
                ic.target = kNoTarget;
                c.wing.push_back(add_unit(ic));
            }
            for (uint32_t id : c.wing) {
                if (id >= units_.size()) continue;
                Unit& ic = units_[id];
                if (!ic.alive) continue;
                if (ic.loaded) {
                    ic.loaded = false;
                    ic.x = host.x;
                    ic.y = host.y;
                }
                ic.orders.clear();
                ic.orders.push_back(Order{OrderType::Attack, Fixed(), Fixed(), target});
            }
        } else {
            for (uint32_t id : c.wing) {
                if (id >= units_.size()) continue;
                Unit& ic = units_[id];
                if (!ic.alive) continue;
                ic.loaded = true;
                ic.orders.clear();
                ic.target = kNoTarget;
            }
        }
    }
}

bool World::launch_nova(uint32_t caster, Fixed tx, Fixed ty) {
    if (nova_launch_reg_.empty() || caster >= units_.size() || !units_[caster].alive) return false;
    auto it = nova_launch_reg_.find(units_[caster].catalog_id);
    if (it == nova_launch_reg_.end()) return false;
    const NovaArm& a = it->second;
    if (ability_cooldown_left(caster, kNovaCooldownSlot).raw() > 0) return false;
    Nova n;
    n.x = units_[caster].x;
    n.y = units_[caster].y;
    n.tx = tx;
    n.ty = ty;
    n.speed = a.speed;
    n.radius = a.radius;
    n.damage = a.damage;
    n.shield_bonus = a.shield_bonus;
    n.remaining = a.duration.raw() > 0 ? a.duration : Fixed::from_int(3);
    n.owner = units_[caster].player;
    n.caster = caster;
    n.active = true;
    novas_.push_back(n);
    if (a.cooldown.raw() > 0) arm_ability_cooldown(caster, kNovaCooldownSlot, a.cooldown);
    return true;
}

void World::step_nova(Fixed dt) {
    if (novas_.empty()) return;
    for (Nova& n : novas_) {
        if (!n.active) continue;
        n.remaining = n.remaining - dt;
        const Fixed dx = n.tx - n.x, dy = n.ty - n.y;
        const Fixed dist = isqrt(dx * dx + dy * dy);
        const Fixed step = n.speed * dt;
        if (dist <= step || dist.raw() <= 0) {
            n.x = n.tx;
            n.y = n.ty;
        } else {
            n.x = n.x + dx * step / dist;
            n.y = n.y + dy * step / dist;
        }
        if (n.remaining.raw() <= 0) {
            const Fixed r2 = n.radius * n.radius;
            const uint32_t k = uint32_t(units_.size());
            for (uint32_t i = 0; i < k; ++i) {
                Unit& u = units_[i];
                if (!u.alive || are_allied(u.player, n.owner)) continue;
                const Fixed ux = u.x - n.x, uy = u.y - n.y;
                if (ux * ux + uy * uy > r2) continue;
                if (n.shield_bonus.raw() > 0 && u.shields.raw() > 0) {
                    const Fixed strip = u.shields < n.shield_bonus ? u.shields : n.shield_bonus;
                    u.shields = u.shields - strip;
                }
                apply_spell_damage(i, n.damage, n.caster);
            }
            n.active = false;
        }
    }
}

void World::step() {
    const Fixed dt = World::dt();
    in_step_ = true;
    update_conjoined();
    return_dead_carry();
    update_destructibles();
    creep_.tick();
    death_reveal_.advance(dt);
    scan_sweep_.advance(dt);
    step_power();
    step_production(dt);
    step_worker_builds();
    step_persistents(dt);
    step_healers(dt);
    for (size_t i = 0; i < stat_mods_.size();) {
        StatModInstance& m = stat_mods_[i];
        m.timer = m.timer - dt;
        if (m.timer.raw() > 0) { ++i; continue; }
        if (m.target < units_.size()) apply_stat_delta(units_[m.target], m.stat, Fixed() - m.delta);
        stat_mods_.erase(stat_mods_.begin() + i);
    }
    for (Unit& u : units_) {
        if (u.ability_cd.empty()) continue;
        const Fixed cd_dt = chrono_dt(u, dt);
        for (size_t i = 0; i < u.ability_cd.size();) {
            u.ability_cd[i].second = u.ability_cd[i].second - cd_dt;
            if (u.ability_cd[i].second.raw() <= 0) u.ability_cd.erase(u.ability_cd.begin() + i);
            else ++i;
        }
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
        bool crushed = false;
        if (ff.radius.raw() > 0) {
            const Fixed r2 = ff.radius * ff.radius;
            for (const Unit& u : units_) {
                if (!u.alive || !u.crushes_ff || u.flying || u.loaded || u.burrowed) continue;
                const Fixed dx = u.x - ff.x, dy = u.y - ff.y;
                if (dx * dx + dy * dy <= r2) { crushed = true; break; }
            }
        }
        if (crushed || ff.timer.raw() <= 0) {
            for (const auto& c : ff.cells) {
                nav_.set_blocked(c.first, c.second, false);
                nav_.mark_ff(c.first, c.second, false);
            }
            force_fields_.erase(force_fields_.begin() + i);
        } else {
            ++i;
        }
    }
    step_auras();
    step_cloak_field();
    step_windup(dt);
    step_point_defense(dt);
    step_projectiles(dt);
    step_interceptors(dt);
    step_nova(dt);
    update_watchtowers();
    for (Unit& u : units_) {
        if (!u.alive) continue;

        const Fixed regen = (u.burrowed && u.burrow_regen.raw() > 0) ? u.burrow_regen : u.life_regen;
        if (regen.raw() > 0 && u.hp.raw() > 0 && u.hp < u.hp_max) {
            u.hp = u.hp + regen * dt;
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
        if (u.barrier_cd.raw() > 0) {
            u.barrier_cd = u.barrier_cd - dt;
            if (u.barrier_cd.raw() < 0) u.barrier_cd = Fixed();
        }
        if (u.time_life.raw() > 0) {
            u.time_life = u.time_life - dt;
            if (u.time_life.raw() <= 0) {
                u.time_life = Fixed();
                u.alive = false;
                continue;
            }
        }
        if (building_burndown_enabled_ && u.burns_down && u.hp.raw() > 0) {
            if (u.hp * Fixed::from_int(3) < u.hp_max) {
                u.burn_timer = u.burn_timer + dt;
                if (u.burn_timer >= kBurnDownPeriod) {
                    u.burn_timer = u.burn_timer - kBurnDownPeriod;
                    u.hp = u.hp - Fixed::from_int(1);
                    if (u.hp.raw() <= 0) { u.hp = Fixed(); u.alive = false; continue; }
                }
            } else {
                u.burn_timer = Fixed();
            }
        }
        if (u.phase_timer.raw() > 0) {
            u.phase_timer = u.phase_timer - dt;
            if (u.phase_timer.raw() <= 0) {
                u.phase_timer = Fixed();
                if (u.phase_shade < units_.size() && units_[u.phase_shade].alive) {
                    u.x = units_[u.phase_shade].x;
                    u.y = units_[u.phase_shade].y;
                    units_[u.phase_shade].alive = false;
                }
                u.phase_shade = kNoTarget;
            }
        }
        if (u.chrono_timer.raw() > 0) {
            u.chrono_timer = u.chrono_timer - dt;
            if (u.chrono_timer.raw() < 0) u.chrono_timer = Fixed();
        }
        if (u.warp_cd.raw() > 0) {
            u.warp_cd = u.warp_cd - chrono_dt(u, dt);
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
            u.morph_timer = u.morph_timer - chrono_dt(u, dt);
            if (u.morph_timer.raw() <= 0 && u.morph_proto < int32_t(spawn_protos_.size())) {
                const uint32_t keep_id = u.id;
                const int32_t keep_player = u.player;
                const Fixed keep_x = u.x, keep_y = u.y;
                const Fixed keep_hp = u.hp, keep_shields = u.shields;
                const Fixed heal_life = u.morph_heal_life_frac, heal_shield = u.morph_heal_shield_frac;
                const int32_t keep_sp = u.spawner_proto, keep_scap = u.spawner_cap;
                const int32_t keep_shard = u.spawner_hard_cap;
                const Fixed keep_sper = u.spawner_period, keep_stimer = u.spawner_timer;
                const int32_t keep_ip = u.inject_proto, keep_icnt = u.inject_count;
                const Fixed keep_itimer = u.inject_timer;
                const Fixed keep_zx = u.zone_x, keep_zy = u.zone_y, keep_zr = u.zone_radius;
                const bool morph_pair = u.morph_pair;
                const int32_t morph_target = u.morph_proto;
                u = spawn_protos_[u.morph_proto];
                u.id = keep_id;
                u.player = keep_player;
                u.x = keep_x;
                u.y = keep_y;
                if (keep_sp >= 0) {
                    u.spawner_proto = keep_sp;
                    u.spawner_cap = keep_scap;
                    u.spawner_hard_cap = keep_shard;
                    u.spawner_period = keep_sper;
                    u.spawner_timer = keep_stimer;
                }
                if (keep_ip >= 0) {
                    u.inject_proto = keep_ip;
                    u.inject_count = keep_icnt;
                    u.inject_timer = keep_itimer;
                }
                if (keep_zr.raw() > 0) {
                    u.zone_x = keep_zx;
                    u.zone_y = keep_zy;
                    u.zone_radius = keep_zr;
                }
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
                if (morph_pair)
                    pending_spawns_.push_back({morph_target, 1, keep_x, keep_y, keep_player, kNoTarget, false});
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

        if (u.loaded) continue;
        if (u.burrowed) {
            if (u.can_burrow_move && !u.orders.empty() && u.orders.front().type == OrderType::Move &&
                !behavior_rooted(u)) {
                const Order o = u.orders.front();
                if (move_unit_toward(u, o.tx, o.ty, move_step(u, dt)))
                    u.orders.erase(u.orders.begin());
                continue;
            }
            if (!u.can_burrow_attack) continue;
        }

        if (u.constructing) {
            step_construction(u, dt);
            continue;
        }

        if (u.addon_building != 0) {
            u.addon_build_left = u.addon_build_left - dt;
            if (u.addon_build_left.raw() <= 0) {
                attach_addon(u.id, u.addon_building);
                u.addon_building = 0;
                u.addon_build_left = Fixed();
            }
        }

        if (u.energy_max.raw() > 0 && u.energy < u.energy_max) {
            u.energy = u.energy + u.energy_regen * dt;
            if (u.energy > u.energy_max) u.energy = u.energy_max;
        }

        if (u.spawner_proto >= 0 && u.spawner_cap > 0 && u.spawner_period.raw() > 0) {
            const bool under_hard = u.spawner_hard_cap <= 0 || larva_total(u.id) < u.spawner_hard_cap;
            if (spawner_brood(u.id) < u.spawner_cap && under_hard) {
                u.spawner_timer = u.spawner_timer - chrono_dt(u, dt);
                if (u.spawner_timer.raw() <= 0) {
                    pending_spawns_.push_back(
                        {u.spawner_proto, 1, u.x, u.y, u.player, u.id, false});
                    u.spawner_timer = u.spawner_period;
                }
            }
        }

        if (u.inject_proto >= 0 && u.inject_count > 0) {
            u.inject_timer = u.inject_timer - chrono_dt(u, dt);
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
                if (b.ctrl_orig_owner != -2) {
                    if (b.ctrl_caster >= units_.size() || !units_[b.ctrl_caster].alive) b.timer = Fixed();
                    if (b.timer.raw() <= 0 && u.alive) u.player = b.ctrl_orig_owner;
                }
                if (b.lift && b.timer.raw() <= 0 && u.alive) u.flying = false;
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

        if (!u.orders.empty() && u.orders.front().type == OrderType::Unload) {
            unload_all(u.id);
            u.orders.erase(u.orders.begin());
            continue;
        }

        if (!u.orders.empty() && u.orders.front().type == OrderType::Load) {
            const uint32_t tid = u.orders.front().target;
            if (tid >= units_.size() || !units_[tid].alive || units_[tid].cargo_space <= 0) {
                u.orders.erase(u.orders.begin());
                continue;
            }
            const Unit& t = units_[tid];
            const Fixed reach = u.radius + t.radius + Fixed::from_ratio(1, 4);
            if (dist2(u, t) <= reach * reach) {
                load_unit(tid, u.id);
                u.orders.erase(u.orders.begin());
            } else {
                move_unit_toward(u, t.x, t.y, move_step(u, dt));
            }
            continue;
        }

        if (u.cooldown.raw() > 0) {
            u.cooldown = u.cooldown - dt;
            if (u.cooldown.raw() < 0) u.cooldown = Fixed();
        }
        if (u.backswing_timer.raw() > 0) {
            u.backswing_timer = u.backswing_timer - dt;
            if (u.backswing_timer.raw() < 0) u.backswing_timer = Fixed();
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

        if (flee_response_enabled_ && u.flees_when_attacked && u.flee_from != kNoTarget &&
            u.orders.empty() && u.hp.raw() > 0 && !u.burrowed && !u.loaded) {
            bool threat_ok = false;
            if (u.flee_from < units_.size()) {
                const Unit& e = units_[u.flee_from];
                if (e.alive && e.player >= 0 && e.player != u.player && e.hp.raw() > 0 && !e.loaded &&
                    !e.burrowed && !e.cloaked_effective() &&
                    (u.sight.raw() <= 0 || dist2(u, e) <= u.sight * u.sight))
                    threat_ok = true;
            }
            if (threat_ok) {
                const Unit& e = units_[u.flee_from];
                Fixed away_x = u.x - e.x, away_y = u.y - e.y;
                if (away_x.raw() == 0 && away_y.raw() == 0) away_x = Fixed::from_int(1);
                u.orders.push_back(Order{OrderType::Move, u.x + away_x, u.y + away_y, kNoTarget});
            } else {
                u.flee_from = kNoTarget;
            }
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
                atk = acquire_target(u, u.acquire_range);
                break;
            case OrderType::AttackMove:
                atk = acquire_target(u, u.acquire_range);
                if (atk == kNoTarget || (fwm_ && u.fire_while_moving)) {
                    has_dest = true;
                    dx_dest = u.orders.front().tx;
                    dy_dest = u.orders.front().ty;
                }
                break;
            case OrderType::Patrol: {
                atk = acquire_target(u, u.acquire_range);
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
                if (u.is_worker || u.flees_when_attacked) break;
                atk = acquire_target(u, u.acquire_range);
                if (leash_) {
                    if (!u.guarding) { u.guard_x = u.x; u.guard_y = u.y; u.guarding = true; }
                    if (atk != kNoTarget) {
                        const Fixed lx = units_[atk].x - u.guard_x, ly = units_[atk].y - u.guard_y;
                        if (lx * lx + ly * ly > u.acquire_range * u.acquire_range)
                            atk = kNoTarget;
                    }
                    if (atk == kNoTarget) {
                        const Fixed hx = u.guard_x - u.x, hy = u.guard_y - u.y;
                        const Fixed slack = u.radius + Fixed::from_ratio(1, 8);
                        if (hx * hx + hy * hy > slack * slack) {
                            has_dest = true;
                            dx_dest = u.guard_x;
                            dy_dest = u.guard_y;
                        }
                    }
                }
                break;
        }
        if (leash_ && !u.orders.empty()) u.guarding = false;

        if (u.hold_fire) atk = kNoTarget;

        if (behavior_rooted(u)) can_move = false;
        if (backswing_ && u.backswing_timer.raw() > 0) can_move = false;
        if (u.burrowed) can_move = false;

        u.target = atk;

        if (atk != kNoTarget && atk < units_.size() && units_[atk].alive) {
            Unit& tgt = units_[atk];
            Fixed w_dmg = u.wpn_damage, w_per = u.wpn_period, w_rng = u.wpn_range;
            Fixed w_min = u.wpn_min_range;
            uint32_t w_battr = u.wpn_bonus_attr;
            Fixed w_bamt = u.wpn_bonus_amount;
            Fixed w_dp = u.wpn_damage_point;
            Fixed w_bs = u.wpn_backswing;
            Fixed w_ps = u.wpn_projectile_speed;
            int w_hits = u.wpn_hits;
            Fixed w_ar = u.wpn_armor_reduction;
            const uint32_t plane_bit = tgt.flying ? 2u : 1u;
            const bool w1_cov = (u.wpn_target_filter & plane_bit) != 0;
            const bool w2_cov = u.wpn2_damage.raw() > 0 && (u.wpn2_target_filter & plane_bit) != 0;
            if (w2_cov && (!w1_cov || u.wpn2_range > u.wpn_range)) {
                w_dmg = u.wpn2_damage;
                w_per = u.wpn2_period;
                w_rng = u.wpn2_range;
                w_min = u.wpn2_min_range;
                w_battr = u.wpn2_bonus_attr;
                w_bamt = u.wpn2_bonus_amount;
                w_dp = u.wpn2_damage_point;
                w_bs = u.wpn2_backswing;
                w_ps = u.wpn2_projectile_speed;
                w_hits = u.wpn2_hits;
                w_ar = u.wpn2_armor_reduction;
            }
            const bool can_hit_plane = w1_cov || w2_cov;
            Fixed reach = w_rng + u.radius + tgt.radius;
            const Fixed d2 = dist2(u, tgt);
            const bool zone_ok =
                u.zone_radius.raw() <= 0 ||
                (!tgt.flying && (tgt.x - u.zone_x) * (tgt.x - u.zone_x) +
                                        (tgt.y - u.zone_y) * (tgt.y - u.zone_y) <=
                                    u.zone_radius * u.zone_radius);
            bool in_range = can_hit_plane && d2 <= reach * reach &&
                            (w_min.raw() <= 0 || d2 >= w_min * w_min) && zone_ok;
            if (in_range) {
                const bool facing_ok = advance_turn(u, tgt.x, tgt.y);
                if (facing_ok && u.cooldown.raw() == 0 && w_dmg.raw() > 0) {
                    if (attack_windup_ && w_dp.raw() > 0) {
                        u.windup_timer = w_dp;
                        u.windup_target = tgt.id;
                        u.windup_dmg = w_dmg;
                        u.windup_ar = w_ar;
                        u.windup_battr = w_battr;
                        u.windup_bamt = w_bamt;
                        u.windup_ps = projectile_travel_ ? w_ps : Fixed();
                    } else if (projectile_travel_ && w_ps.raw() > 0) {
                        queue_projectile(u.id, u.x, u.y, tgt.id, w_ps, w_dmg, w_battr, w_bamt, w_ar);
                    } else {
                        apply_weapon_fire(u.id, tgt.id, w_dmg, w_battr, w_bamt, w_hits, w_ar);
                    }
                    u.cooldown = u.atk_speed_mult.raw() == Fixed::from_int(1).raw()
                                     ? w_per
                                     : w_per / u.atk_speed_mult;
                    if (backswing_) u.backswing_timer = w_bs;
                }
                if (fwm_ && u.fire_while_moving && has_dest && can_move)
                    move_unit_toward(u, dx_dest, dy_dest, move_step(u, dt));
            } else if (can_move) {
                if (fwm_ && u.fire_while_moving && has_dest)
                    move_unit_toward(u, dx_dest, dy_dest, move_step(u, dt));
                else
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
                const bool is_shade = ps.phase_link != kNoTarget;
                const bool exact = is_shade || ps.gather_node != kNoTarget;
                s.x = exact ? ps.x : ps.x + Fixed::from_int(DX[i & 7]);
                s.y = exact ? ps.y : ps.y + Fixed::from_int(DY[i & 7]);
                s.alive = true;
                if (ps.gather_node != kNoTarget) {
                    s.gather_target = ps.gather_node;
                    s.home_depot = ps.depot;
                }
                const uint32_t sid = add_unit(s);
                if (ps.gather_node != kNoTarget) issue(sid, Order{OrderType::Gather});
                if (is_shade && ps.phase_link < units_.size()) {
                    units_[ps.phase_link].phase_shade = sid;
                    issue(sid, Order{OrderType::Move, ps.move_x, ps.move_y});
                }
            }
        }
        pending_spawns_.clear();
    }

    if (acceleration_) {
        for (Unit& u : units_) {
            if (u.alive && !u.moving_now && u.cur_speed.raw() > 0) {
                const Fixed d = u.decel.raw() > 0 ? u.decel : u.accel;
                u.cur_speed = u.cur_speed - d * dt;
                if (u.cur_speed.raw() < 0) u.cur_speed = Fixed();
            }
            u.moving_now = false;
        }
    }
    apply_separation();
    in_step_ = false;
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
    for (const Carrier& c : carriers_) {
        h = mix(h, c.carrier);
        h = mix(h, uint32_t(c.count));
        h = mix(h, uint32_t(c.count_max));
        h = mix(h, uint32_t(c.build_timer.raw()));
        for (uint32_t id : c.wing) h = mix(h, id);
    }
    for (const Nova& n : novas_) {
        if (!n.active) continue;
        h = mix(h, uint32_t(n.x.raw()));
        h = mix(h, uint32_t(n.y.raw()));
        h = mix(h, uint32_t(n.remaining.raw()));
        h = mix(h, uint32_t(n.damage.raw()));
        h = mix(h, uint32_t(n.owner));
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
        h = mix(h, u.flying ? 2u : 0u);
        if (u.lowered) h = mix(h, 0x10ED0u);
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
        if (u.barrier_max.raw() > 0) {
            h = mix(h, uint32_t(u.barrier.raw()));
            h = mix(h, uint32_t(u.barrier_cd.raw()));
        }
        if (u.buff_timer.raw() != 0) {
            h = mix(h, uint32_t(u.buff_timer.raw()));
            h = mix(h, uint32_t(u.buff_damage.raw()));
        }
        if (turn_limit_) h = mix(h, uint32_t(u.facing.raw()));
        if (acceleration_) h = mix(h, uint32_t(u.cur_speed.raw()));
        if (leash_) {
            h = mix(h, uint32_t(u.guard_x.raw()));
            h = mix(h, uint32_t(u.guard_y.raw()));
            h = mix(h, u.guarding ? 1u : 0u);
        }
        if (u.windup_timer.raw() > 0) {
            h = mix(h, uint32_t(u.windup_timer.raw()));
            h = mix(h, u.windup_target);
            h = mix(h, uint32_t(u.windup_dmg.raw()));
            h = mix(h, u.windup_battr);
            h = mix(h, uint32_t(u.windup_bamt.raw()));
            h = mix(h, uint32_t(u.windup_ps.raw()));
            h = mix(h, uint32_t(u.windup_ar.raw()));
        }
        if (u.backswing_timer.raw() > 0) h = mix(h, uint32_t(u.backswing_timer.raw()));
        if (u.constructing) {
            h = mix(h, uint32_t(u.build_progress.raw()));
            h = mix(h, uint32_t(u.build_time.raw()));
            if (u.needs_builder) h = mix(h, u.builder);
        }
        if (u.construct_target != kNoTarget) h = mix(h, u.construct_target);
        if (u.addon_building != 0) {
            h = mix(h, uint32_t(u.addon_building));
            h = mix(h, uint32_t(u.addon_build_left.raw()));
        }
        if (u.flee_from != kNoTarget) h = mix(h, u.flee_from);
        if (u.energy_max.raw() != 0) h = mix(h, uint32_t(u.energy.raw()));
        for (const auto& cd : u.ability_cd) {
            h = mix(h, uint32_t(cd.first));
            h = mix(h, uint32_t(cd.second.raw()));
        }
        if (u.cloaked) h = mix(h, 0xC10Au);
        if (u.hold_fire) h = mix(h, 0x40F1u);
        if (u.chrono_timer.raw() != 0) h = mix(h, uint32_t(u.chrono_timer.raw()));
        if (u.shields_max.raw() != 0) h = mix(h, uint32_t(u.shields_max.raw()));
        if (u.shield_regen_rate.raw() > 0 && u.shields_max.raw() > 0)
            h = mix(h, uint32_t(u.shield_since_hit.raw()));
        if (u.atk_speed_mult.raw() != Fixed::from_int(1).raw())
            h = mix(h, uint32_t(u.atk_speed_mult.raw()));
        if (u.damage_reduction.raw() != 0) h = mix(h, uint32_t(u.damage_reduction.raw()));
        if (u.dmg_fraction.raw() != Fixed::from_int(1).raw()) h = mix(h, uint32_t(u.dmg_fraction.raw()));
        if (u.dmg_clamp_max.raw() != 0) h = mix(h, uint32_t(u.dmg_clamp_max.raw()));
        if (u.dmg_minimum.raw() != 0) h = mix(h, uint32_t(u.dmg_minimum.raw()));
        if (u.aura_reduction.raw() != 0) h = mix(h, uint32_t(u.aura_reduction.raw()));
        if (u.needs_power) h = mix(h, u.powered ? 0x50E1u : 0x50E0u);
        if (u.burns_down && u.burn_timer.raw() > 0) h = mix(h, uint32_t(u.burn_timer.raw()));
        if (u.loaded) { h = mix(h, 0x10ADu); h = mix(h, u.transport_id); }
        if (u.burrowed) h = mix(h, 0xB020u);
        if (u.burrowed_wpn_on) h = mix(h, 0xB021u);
        if (u.field_cloaked) h = mix(h, 0xC10Au);
        if (u.time_life.raw() > 0) h = mix(h, uint32_t(u.time_life.raw()));
        if (u.phase_timer.raw() > 0) h = mix(h, uint32_t(u.phase_timer.raw()) ^ 0xADE7u);
        if (u.morph_proto >= 0) {
            h = mix(h, uint32_t(u.morph_timer.raw()));
            h = mix(h, uint32_t(u.morph_proto));
            h = mix(h, uint32_t(u.morph_heal_life_frac.raw()));
            h = mix(h, uint32_t(u.morph_heal_shield_frac.raw()));
            if (u.morph_pair) h = mix(h, 0x9A17u);
        }
        if (u.zone_radius.raw() > 0) {
            h = mix(h, uint32_t(u.zone_x.raw()));
            h = mix(h, uint32_t(u.zone_y.raw()));
            h = mix(h, uint32_t(u.zone_radius.raw()) ^ 0x71B3u);
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
            h = mix(h, uint32_t((b.root ? 1u : 0u) | (b.stun ? 2u : 0u) | (b.lift ? 4u : 0u)));
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
    for (const HealerInstance& hb : healers_) {
        h = mix(h, hb.caster);
        h = mix(h, hb.target);
    }
    for (const PendingHit& p : pending_hits_) {
        h = mix(h, p.attacker);
        h = mix(h, p.target);
        h = mix(h, uint32_t(p.px.raw()));
        h = mix(h, uint32_t(p.py.raw()));
        h = mix(h, uint32_t(p.speed.raw()));
        h = mix(h, uint32_t(p.dmg.raw()));
        h = mix(h, p.battr);
        h = mix(h, uint32_t(p.bamt.raw()));
        h = mix(h, uint32_t(p.life.raw()));
        h = mix(h, uint32_t(p.ar.raw()));
    }
    h = mix(h, creep_.checksum());
    return h;
}

}
