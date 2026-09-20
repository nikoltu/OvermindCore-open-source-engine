#include "sc2/sim/world.hpp"

#include <algorithm>

#include "sc2/sim/build.hpp"
#include "sc2/sim/combat.hpp"
#include "sc2/sim/weapon.hpp"

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

int World::defeat_holders(int player) const {
    int n = 0;
    for (const Unit& u : units_)
        if (u.alive && u.player == player && (!defeat_rule_authored_ || u.prevent_defeat)) ++n;
    return n;
}

int World::winner() const {
    int found = -1;
    for (const Unit& u : units_) {
        if (!u.alive || u.player < 0) continue;
        if (defeat_rule_authored_ && !u.prevent_defeat) continue;
        if (found == -1)
            found = u.player;
        else if (found != u.player)
            return -1;
    }
    return found;
}

bool World::visible_to(int32_t player, const Unit& target) const {
    if (target.reveal_timer.raw() > 0 && target.reveal_to >= 0 && are_allied(target.reveal_to, player)) return true;
    for (const Unit& a : units_) {
        if (!a.alive || a.sight.raw() <= 0) continue;
        const bool by_team = are_allied(a.player, player);
        const bool by_grant = a.vision_player >= 0 && are_allied(a.vision_player, player);
        if (!by_team && !by_grant) continue;
        const Fixed dx = target.x - a.x, dy = target.y - a.y;
        if (dx * dx + dy * dy > a.sight * a.sight) continue;
        if (placement_.configured()) {
            const bool sees_over = a.flying || a.vision_height.raw() >= Fixed::from_int(15).raw();
            const std::uint8_t viewer_level =
                sees_over ? std::uint8_t(0xFF) : placement_.level_at(a.x, a.y);
            int tx = 0, ty = 0;
            placement_.to_cell(target.x, target.y, tx, ty);
            if (!placement_.vision_reaches(viewer_level, tx, ty)) continue;
        }
        return true;
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

void World::release_dead_creep_sources() {
    for (Unit& u : units_) {
        if (u.alive || u.creep_source_idx < 0) continue;
        creep_.kill_source(u.creep_source_idx);
        u.creep_source_idx = -1;
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
    destructibles_.erase(std::remove_if(destructibles_.begin(), destructibles_.end(),
                                        [](const Destructible& d) { return d.cleared; }),
                         destructibles_.end());
    for (auto it = morph_founder_.begin(); it != morph_founder_.end();) {
        const bool live = it->first < units_.size() && units_[it->first].alive && units_[it->first].constructing;
        it = live ? std::next(it) : morph_founder_.erase(it);
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
    if (power_disabled(u)) return kNoTarget;
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
    if (u.wpn_damage.raw() <= 0 && u.wpn2_damage.raw() <= 0 && !u.has_burrowed_wpn && !hosts_interceptors(u.id))
        return kNoTarget;
    for (const Unit& o : units_) {
        if (!o.alive || are_allied(o.player, u.player)) continue;
        if (o.resource_left > 0) continue;
        if (o.loaded) continue;
        if (untargetable(o)) continue;
        const uint32_t plane_bit = o.flying ? 2u : 1u;
        const bool w1 = (u.wpn_damage.raw() > 0 || u.has_burrowed_wpn || hosts_interceptors(u.id)) &&
                        (u.wpn_target_filter & plane_bit) != 0;
        const bool w2 = u.wpn2_damage.raw() > 0 && (u.wpn2_target_filter & plane_bit) != 0;
        if (!w1 && !w2) continue;
        if (u.zone_radius.raw() > 0) {
            if (o.flying) continue;
            const Fixed zx = o.x - u.zone_x, zy = o.y - u.zone_y;
            if (zx * zx + zy * zy > u.zone_radius * u.zone_radius) continue;
        }
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
        if ((o.burrowed || o.cloaked_effective()) && !detected_by(u.player, o)) continue;
        if (fog_acquire_ && !visible_to(u.player, o)) continue;
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
constexpr Fixed kContactSlack = Fixed::from_raw(252);

inline Fixed arrival_near(const Unit& u) { return u.radius + Fixed::from_ratio(1, 8); }

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

Fixed World::move_step(Unit& u, Fixed dt, Fixed brake_dist) {
    Fixed speed = (u.flying && u.speed_air.raw() > 0) ? u.speed_air : u.speed;
    if (creep_.is_creep(u.x, u.y)) speed = speed * u.speed_mult_creep;
    if (!u.behaviors.empty()) speed = speed * behavior_speed_mult(u);
    if (!acceleration_ || u.accel.raw() <= 0) return speed * dt;
    u.moving_now = true;
    if (brake_dist.raw() >= 0) {
        const Fixed d = u.decel.raw() > 0 ? u.decel : u.accel;
        const Fixed cap = isqrt(Fixed::from_int(2) * d * brake_dist) + d * dt;
        if (cap < speed) speed = cap;
    }
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

bool World::advance_turn(Unit& u, Fixed tx, Fixed ty, Fixed fire_arc_deg) {
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
    if (arc_movement_ && acceleration_ && !u.from_rest && u.cur_speed.raw() > 0 && u.lateral_accel.raw() > 0) {
        Fixed budget = (u.lateral_accel * World::dt()) / u.cur_speed;
        if (u.turn_rate.raw() > 0) {
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
    const Fixed rate = (acceleration_ && u.from_rest) ? u.stationary_turn_rate : u.turn_rate;
    if (rate.raw() <= 0) return true;
    const Fixed budget = (rate * World::dt()) * Fixed::from_raw(kPi) / Fixed::from_int(180);
    if (budget.raw() <= 0) return true;
    const int32_t mag = err.raw() >= 0 ? err.raw() : -err.raw();
    if (mag <= budget.raw()) {
        u.facing = desired;
    } else {
        u.facing = wrap(u.facing + (err.raw() >= 0 ? budget : Fixed() - budget));
    }
    const int32_t rem_mag = [&] { const int32_t r = wrap(desired - u.facing).raw(); return r >= 0 ? r : -r; }();
    const int32_t tol = fire_arc_deg.raw() > 0
                            ? int32_t(std::int64_t(kPi) * fire_arc_deg.raw() /
                                      (std::int64_t(180) * Fixed::from_int(1).raw()))
                            : kPi / 16;
    return rem_mag <= tol;
}

static Fixed raw_length(Fixed dx, Fixed dy);

void World::displace_from_force_field(const ForceFieldInstance& ff) {
    if (ff.radius.raw() <= 0) return;
    for (Unit& u : units_) {
        if (!u.alive || u.flying || u.crushes_ff || u.loaded || (u.attributes & kAttrStructure)) continue;
        const Fixed dx = u.x - ff.x, dy = u.y - ff.y;
        if (dx * dx + dy * dy >= ff.radius * ff.radius) continue;
        const Fixed rim = ff.radius + u.radius - kContactSlack;
        const Fixed len = raw_length(dx, dy);
        if (len.raw() == 0) {
            u.x = ff.x + rim;
            continue;
        }
        u.x = ff.x + dx * rim / len;
        u.y = ff.y + dy * rim / len;
    }
}

bool World::move_unit_toward(Unit& u, Fixed gx, Fixed gy, Fixed step) {
    const Fixed from_x = u.x, from_y = u.y;
    if (u.loco_moved.raw() == 0) {
        u.loco_x0 = from_x;
        u.loco_y0 = from_y;
        u.arrived = false;
    }
    const bool done = move_unit_toward_impl(u, gx, gy, step);
    u.loco_moved = u.loco_moved + raw_length(u.x - from_x, u.y - from_y);
    return done;
}

bool World::move_unit_toward_impl(Unit& u, Fixed gx, Fixed gy, Fixed step) {
    u.moved_this_tick = true;
    if (!nav_.has_blocked() || u.flying) {
        u.move_target_x = gx;
        u.move_target_y = gy;
        if (!advance_turn(u, gx, gy)) {
            u.cur_speed = Fixed();
            return false;
        }
        if (arc_movement_ && acceleration_ && !u.from_rest && u.cur_speed.raw() > 0 && u.lateral_accel.raw() > 0) {
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
                           u.path_gx != gcx || u.path_gy != gcy || u.path_rev != nav_.revision();
    if (need_plan) {
        std::vector<NavCell> cells;
        find_path(nav_, scx, scy, gcx, gcy, cells, u.can_jump, u.crushes_ff, u.walks_cliffs, &gx, &gy, u.radius);
        ++nav_plans_;
        if (cells.empty()) ++nav_plan_failures_;
        u.path.clear();
        u.path_next = 0;
        u.path_gx = gcx;
        u.path_gy = gcy;
        u.path_rev = nav_.revision();
        for (size_t k = 1; k < cells.size(); ++k) {
            Fixed wx, wy;
            nav_.cell_center(cells[k].x, cells[k].y, wx, wy);
            u.path.push_back(Waypoint{wx, wy});
        }
        if (!u.path.empty()) {
            u.path.back() = Waypoint{gx, gy};
            const NavCell& last = cells.back();
            if (last.x != gcx || last.y != gcy) {
                Fixed lx0, ly0;
                nav_.cell_center(last.x, last.y, lx0, ly0);
                const Fixed half = nav_.cell_size() / Fixed::from_int(2);
                const Fixed eps = Fixed::from_raw(1);
                const Fixed cx0 = lx0 - half, cx1 = lx0 + half - eps, cy0 = ly0 - half, cy1 = ly0 + half - eps;
                u.path.back() = Waypoint{gx < cx0 ? cx0 : (gx > cx1 ? cx1 : gx), gy < cy0 ? cy0 : (gy > cy1 ? cy1 : gy)};
            }
        } else
            u.path.push_back(Waypoint{gx, gy});
        if (!cells.empty() && u.path.size() > 1) {
            std::vector<Waypoint> pulled;
            Fixed axp = u.x, ayp = u.y;
            std::size_t k = 0;
            while (k < u.path.size()) {
                std::size_t j = k;
                while (j + 1 < u.path.size() &&
                       nav_.segment_walkable(axp, ayp, u.path[j + 1].x, u.path[j + 1].y, u.walks_cliffs))
                    ++j;
                pulled.push_back(u.path[j]);
                axp = u.path[j].x;
                ayp = u.path[j].y;
                k = j + 1;
            }
            u.path.swap(pulled);
        }
    }

    if (u.path_next < u.path.size()) {
        const Waypoint wp = u.path[u.path_next];
        u.move_target_x = wp.x;
        u.move_target_y = wp.y;
        if (!advance_turn(u, wp.x, wp.y)) {
            u.cur_speed = Fixed();
            return false;
        }
        const bool guard_cells = nav_.has_blocked() && !u.flying;
        int fcx = 0, fcy = 0;
        if (guard_cells) nav_.to_cell(u.x, u.y, fcx, fcy);
        const Fixed prev_x = u.x, prev_y = u.y;
        const bool arrived = move_toward(u, wp.x, wp.y, step);
        if (guard_cells && !nav_.is_blocked_cell(fcx, fcy)) {
            int tcx = 0, tcy = 0;
            nav_.to_cell(u.x, u.y, tcx, tcy);
            const bool crushable_here =
                u.crushes_ff && nav_.has_ff() && nav_.is_ff_cell(tcx, tcy);
            if ((tcx != fcx || tcy != fcy) && !crushable_here && nav_.is_blocked_cell(tcx, tcy)) {
                const Fixed full_x = u.x, full_y = u.y;
                u.x = prev_x;
                u.y = prev_y;
                auto free_at = [&](Fixed nx, Fixed ny) {
                    int qx = 0, qy = 0;
                    nav_.to_cell(nx, ny, qx, qy);
                    return !nav_.is_blocked_cell(qx, qy);
                };
                if (full_x.raw() != prev_x.raw() && free_at(full_x, prev_y)) {
                    u.x = full_x;
                } else if (full_y.raw() != prev_y.raw() && free_at(prev_x, full_y)) {
                    u.y = full_y;
                }
                return false;
            }
        }
        if (arrived) {
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

static bool anchor_zone_occupied(const std::vector<Unit>& units, const Unit& u) {
    const Fixed us = u.radius;
    for (const Unit& o : units) {
        if (&o == &u) continue;
        if (!o.alive || o.flying || o.burrowed) continue;
        if (!o.orders.empty() || o.moved_this_tick || o.pushed_this_tick || o.push_release != 0)
            continue;
        if (u.collide_flags != 0 && o.collide_flags != 0 &&
            (u.collide_flags & o.collide_flags) == 0) continue;
        const Fixed os = o.radius;
        const Fixed zone = us + os - Fixed::from_raw(252);
        const Fixed ddx = o.x - u.anchor_x, ddy = o.y - u.anchor_y;
        if (ddx * ddx + ddy * ddy < zone * zone) return true;
    }
    return false;
}

static Fixed raw_length(Fixed dx, Fixed dy) {
    const std::int64_t q = std::int64_t(dx.raw()) * dx.raw() + std::int64_t(dy.raw()) * dy.raw();
    if (q <= 0) return Fixed();
    std::int64_t lo = 0, hi = 0x7FFFFFFF;
    while (lo < hi) {
        const std::int64_t mid = lo + ((hi - lo + 1) >> 1);
        if (mid * mid <= q) lo = mid; else hi = mid - 1;
    }
    return Fixed::from_raw(std::int32_t(lo));
}

void World::apply_separation() {
    if (!separation_) return;
    const size_t n = units_.size();
    for (size_t i = 0; i < n; ++i) {
        Unit& u = units_[i];
        if (!u.alive || !u.has_anchor) continue;
        if (u.flying) continue;
        const bool own_walk_home =
            u.orders.size() == 1 && u.orders.front().type == OrderType::Move &&
            u.orders.front().tx.raw() == u.anchor_x.raw() &&
            u.orders.front().ty.raw() == u.anchor_y.raw();
        if (!u.orders.empty() && !own_walk_home) { u.has_anchor = false; continue; }
        if (u.push_release != 0) continue;
        if (anchor_zone_occupied(units_, u)) {
            u.has_anchor = false;
            if (own_walk_home) {
                u.orders.clear();
                u.moved_this_tick = false;
            }
        }
    }
    constexpr Fixed kSlack = kContactSlack;
    sep_budget_.resize(n);
    sep_allowance_.resize(n);
    sep_x0_.resize(n);
    sep_y0_.resize(n);
    std::vector<Fixed>& budget = sep_budget_;
    std::vector<Fixed>& allowance = sep_allowance_;
    std::vector<Fixed>& pass_x0 = sep_x0_;
    std::vector<Fixed>& pass_y0 = sep_y0_;
    for (size_t i = 0; i < n; ++i) {
        const Unit& u = units_[i];
        const Fixed step = u.speed * dt();
        allowance[i] = step;
        budget[i] = step;
        const bool walked = u.loco_moved.raw() > 0;
        pass_x0[i] = walked ? u.loco_x0 : u.x;
        pass_y0[i] = walked ? u.loco_y0 : u.y;
    }
    for (size_t i = 0; i < n; ++i) {
        Unit& m = units_[i];
        if (!m.alive || m.burrowed || m.flying || !m.moved_this_tick) continue;
        if (m.speed.raw() == 0) continue;
        Fixed tdx = m.move_target_x - m.x, tdy = m.move_target_y - m.y;
        Fixed t2 = tdx * tdx + tdy * tdy;
        if (t2.raw() <= 0 && turn_limit_) {
            fixed_sincos(m.facing, tdx, tdy);
            t2 = tdx * tdx + tdy * tdy;
        }
        if (t2.raw() <= 0) continue;
        const Fixed tlen = raw_length(tdx, tdy);
        if (tlen.raw() <= 0) continue;
        const Fixed ux = tdx / tlen, uy = tdy / tlen;
        const Fixed lx = Fixed() - uy, ly = ux;
        for (size_t j = 0; j < n; ++j) {
            if (j == i) continue;
            Unit& b = units_[j];
            if (!b.alive || b.burrowed || b.flying) continue;
            if (b.speed.raw() == 0) continue;
            if (b.construct_target != kNoTarget) continue;
            if (m.collide_flags != 0 && b.collide_flags != 0 &&
                (m.collide_flags & b.collide_flags) == 0) continue;
            if (harvesting(m) && harvesting(b) && b.moved_this_tick) continue;
            const Fixed ms = m.sep_radius.raw() > 0 ? m.sep_radius : m.radius;
            const Fixed bs = b.sep_radius.raw() > 0 ? b.sep_radius : b.radius;
            const Fixed sum = ms + bs;
            const Fixed contact = m.radius + b.radius;
            if (sum.raw() <= 0) continue;
            const Fixed dx = b.x - m.x, dy = b.y - m.y;
            const bool coincident = (dx.raw() == 0 && dy.raw() == 0);
            if (!coincident && (dx * ux + dy * uy).raw() <= 0) continue;
            const Fixed d2 = dx * dx + dy * dy;
            const Fixed reach = contact - kSlack;
            if (reach.raw() <= 0) continue;
            if (d2 >= reach * reach) continue;
            Fixed lat = dx * lx + dy * ly;
            const Fixed sign = lat.raw() >= 0 ? Fixed::from_int(1) : Fixed::from_int(-1);
            if (lat.raw() < 0) lat = Fixed() - lat;
            if (lat >= sum) continue;
            const Fixed need = sum - lat;
            const bool m_stiff = m.allied_push_priority >= 1;
            const bool b_stiff = b.allied_push_priority >= 1;
            Fixed share_m, share_b;
            if (m_stiff == b_stiff) {
                share_m = need / Fixed::from_int(2);
                share_b = need - share_m;
            } else if (m_stiff) {
                share_m = Fixed(); share_b = need;
            } else {
                share_m = need; share_b = Fixed();
            }
            const Fixed mcap = budget[i];
            const Fixed bcap = budget[j];
            if (share_m > mcap) share_m = mcap;
            if (share_b > bcap) share_b = bcap;
            budget[i] = budget[i] - share_m;
            budget[j] = budget[j] - share_b;
            const Fixed pre_x = b.x, pre_y = b.y;
            m.x = m.x - lx * sign * share_m;
            m.y = m.y - ly * sign * share_m;
            b.x = b.x + lx * sign * share_b;
            b.y = b.y + ly * sign * share_b;
            if (share_b.raw() > 0) {
                if (!b.has_anchor && b.orders.empty() && !b.moved_this_tick &&
                    b.push_release == 0) {
                    b.has_anchor = true;
                    b.anchor_x = pre_x;
                    b.anchor_y = pre_y;
                }
                b.pushed_this_tick = true;
                b.push_dir_x = lx * sign;
                b.push_dir_y = ly * sign;
            }
        }
    }
    for (size_t i = 0; i < n; ++i) {
        Unit& m = units_[i];
        if (!m.alive || m.burrowed || m.flying || !m.moved_this_tick || m.loco_moved.raw() <= 0) continue;
        if (m.orders.empty()) continue;
        const Order& mo = m.orders.front();
        if (mo.type != OrderType::Move && mo.type != OrderType::AttackMove) continue;
        for (size_t j = 0; j < n; ++j) {
            if (j == i) continue;
            const Unit& b = units_[j];
            if (!b.alive || b.burrowed || b.flying || b.speed.raw() == 0) continue;
            if (!b.orders.empty() || b.moved_this_tick || b.pushed_this_tick) continue;
            if (!b.arrived || !(b.arrive_x == mo.tx) || !(b.arrive_y == mo.ty)) continue;
            if (!are_allied(b.player, m.player)) continue;
            {
                const Fixed gx = mo.tx - b.x, gy = mo.ty - b.y, blob = (m.radius + b.radius) * Fixed::from_int(4);
                if (gx * gx + gy * gy > blob * blob) continue;
            }
            const Fixed contact = m.radius + b.radius - kSlack;
            if (contact.raw() <= 0) continue;
            const Fixed c2 = contact * contact;
            const Fixed dx1 = m.x - b.x, dy1 = m.y - b.y;
            if (dx1 * dx1 + dy1 * dy1 >= c2) continue;
            const Fixed dx0 = m.loco_x0 - b.x, dy0 = m.loco_y0 - b.y;
            if (dx0 * dx0 + dy0 * dy0 < c2) continue;
            const Fixed vx = m.x - m.loco_x0, vy = m.y - m.loco_y0;
            int32_t lo = 0, hi = Fixed::from_int(1).raw();
            for (int it = 0; it < 12; ++it) {
                const int32_t mid = (lo + hi) / 2;
                const Fixed s = Fixed::from_raw(mid);
                const Fixed px = m.loco_x0 + vx * s - b.x, py = m.loco_y0 + vy * s - b.y;
                if (px * px + py * py >= c2) lo = mid; else hi = mid;
            }
            const Fixed s = Fixed::from_raw(lo);
            m.x = m.loco_x0 + vx * s;
            m.y = m.loco_y0 + vy * s;
        }
    }
    for (size_t i = 0; i < n; ++i) {
        Unit& u = units_[i];
        if (!u.alive || u.flying || u.burrowed) continue;
        if (u.pushed_this_tick) { u.push_release = 2; continue; }
        if (u.push_release == 2) {
            const Fixed step = u.speed / Fixed::from_int(32);
            u.x = u.x + u.push_dir_x * step;
            u.y = u.y + u.push_dir_y * step;
            u.push_release = 1;
            continue;
        }
        if (u.push_release == 1) {
            const Fixed step = u.speed / Fixed::from_int(64);
            u.x = u.x + u.push_dir_x * step;
            u.y = u.y + u.push_dir_y * step;
            u.push_release = 0;
            if (u.has_anchor) {
                if (anchor_zone_occupied(units_, u)) {
                    u.has_anchor = false;
                } else {
                    issue(u.id, Order{OrderType::Move, u.anchor_x, u.anchor_y, kNoTarget}, false,
                          false);
                }
            }
            continue;
        }
        if (u.has_anchor && u.orders.empty()) {
            if (move_toward(u, u.anchor_x, u.anchor_y, u.speed * dt()))
                u.has_anchor = false;
        }
    }
    for (size_t i = 0; i < n; ++i) {
        Unit& a = units_[i];
        if (!a.alive || a.flying || a.burrowed || a.speed.raw() == 0) continue;
        if (!a.orders.empty() || !a.arrived || a.moved_this_tick) continue;
        for (size_t j = i + 1; j < n; ++j) {
            Unit& b = units_[j];
            if (!b.alive || b.flying || b.burrowed || b.speed.raw() == 0) continue;
            if (!b.orders.empty() || !b.arrived || b.moved_this_tick) continue;
            if (!are_allied(a.player, b.player)) continue;
            if (a.collide_flags != 0 && b.collide_flags != 0 && (a.collide_flags & b.collide_flags) == 0) continue;
            const Fixed contact = a.radius + b.radius - kContactSlack;
            if (contact.raw() <= 0) continue;
            Fixed dx = b.x - a.x, dy = b.y - a.y;
            Fixed d2 = dx * dx + dy * dy;
            if (d2 >= contact * contact) continue;
            if (d2.raw() <= 0) {
                dx = Fixed::from_raw(1);
                dy = Fixed();
                d2 = dx * dx;
            }
            const Fixed d = raw_length(dx, dy);
            if (d.raw() <= 0) continue;
            const Fixed half = (contact - d) / Fixed::from_int(2);
            const Fixed ux = dx / d, uy = dy / d;
            const Fixed step_a = a.speed * dt(), step_b = b.speed * dt();
            const Fixed sa = half < step_a ? half : step_a, sb = half < step_b ? half : step_b;
            a.x = a.x - ux * sa;
            a.y = a.y - uy * sa;
            b.x = b.x + ux * sb;
            b.y = b.y + uy * sb;
        }
    }

    static constexpr std::uint8_t kAirPartnerFlag = 0x80;
    static constexpr std::int32_t kAirPeakMax = 7;
    auto air_quantum = [](const Unit& u) { return u.speed.raw() / 256; };
    static constexpr std::int32_t kAirOnsetPeriod = 8;
    if ((tick_ + 1) % kAirOnsetPeriod == 0) {
        for (size_t i = 0; i < n; ++i) {
            Unit& a = units_[i];
            if (!a.alive || !a.flying || a.burrowed || a.loaded) continue;
            if (a.moved_this_tick || a.air_expand) continue;
            if (tick_ - a.air_onset_tick < kAirOnsetPeriod - 1) continue;
            if (a.speed.raw() == 0) continue;
            for (size_t j = i + 1; j < n; ++j) {
                Unit& b = units_[j];
                if (!b.alive || !b.flying || b.burrowed || b.loaded) continue;
                if (b.moved_this_tick || b.air_expand) continue;
                if (tick_ - b.air_onset_tick < kAirOnsetPeriod - 1) continue;
                if (b.speed.raw() == 0) continue;
                if (a.collide_flags != 0 && b.collide_flags != 0 &&
                    (a.collide_flags & b.collide_flags) == 0) continue;
                const Fixed as = a.sep_radius.raw() > 0 ? a.sep_radius : a.radius;
                const Fixed bs = b.sep_radius.raw() > 0 ? b.sep_radius : b.radius;
                const Fixed sum = as + bs;
                if (sum.raw() <= 0) continue;
                const Fixed dx = b.x - a.x, dy = b.y - a.y;
                const Fixed d = raw_length(dx, dy);
                if (d >= sum) continue;
                if (d.raw() <= 0) continue;
                const Fixed nx = dx / d, ny = dy / d;
                const std::int32_t ua = air_quantum(a), ub = air_quantum(b);
                if (ua <= 0 && ub <= 0) continue;
                std::int32_t peak = 1, cumulative = 0;
                for (; peak <= kAirPeakMax; ++peak) {
                    cumulative += peak * ua + (peak - 1) * ub;
                    if (cumulative >= kSlack.raw()) break;
                }
                if (peak > kAirPeakMax) peak = kAirPeakMax;
                const std::uint8_t ticks = std::uint8_t(2 * peak - 1);
                const std::uint8_t head = std::uint8_t(peak << 4);
                a.air_expand = std::uint8_t(ticks | head);
                a.push_dir_x = Fixed() - nx;
                a.push_dir_y = Fixed() - ny;
                b.air_expand = std::uint8_t(ticks | head | kAirPartnerFlag);
                b.push_dir_x = nx;
                b.push_dir_y = ny;
                break;
            }
        }
    }
    for (size_t i = 0; i < n; ++i) {
        Unit& u = units_[i];
        if (u.air_expand == 0) continue;
        if (!u.alive || !u.flying) { u.air_expand = 0; continue; }
        const std::int32_t left = std::int32_t(u.air_expand & 0x0F);
        const std::int32_t peak = std::int32_t((u.air_expand >> 4) & 0x07);
        std::int32_t level = left <= peak ? left : 2 * peak - left;
        if (u.air_expand & kAirPartnerFlag) --level;
        if (level > 0) {
            const Fixed step = Fixed::from_raw(level * air_quantum(u));
            u.x = u.x + u.push_dir_x * step;
            u.y = u.y + u.push_dir_y * step;
        }
        u.air_expand = std::uint8_t((left - 1) | (u.air_expand & 0xF0));
        if ((u.air_expand & 0x0F) == 0) u.air_expand = 0;
    }
    for (size_t i = 0; i < n; ++i) {
        Unit& u = units_[i];
        if (!u.alive || u.speed.raw() == 0) continue;
        const Fixed allow = allowance[i];
        const Fixed dx = u.x - pass_x0[i], dy = u.y - pass_y0[i];
        const Fixed travelled = raw_length(dx, dy);
        if (travelled <= allow || travelled.raw() <= 0) continue;
        u.x = pass_x0[i] + (dx / travelled) * allow;
        u.y = pass_y0[i] + (dy / travelled) * allow;
    }
    for (Unit& u : units_) {
        u.moved_this_tick = false;
        u.pushed_this_tick = false;
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
        if (actor < units_.size() && units_[actor].is_worker && t.repair_time.raw() > 0 &&
            (t.attributes & kAttrMechanical) && t.hp < t.hp_max) {
            Order o;
            o.type = OrderType::Repair;
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
    Fixed cap = (p >= 0 && std::size_t(p) < supply_ceiling_.size()) ? supply_ceiling_[std::size_t(p)]
                                                                      : Fixed::from_int(200);
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
    n.beh_suppress_attack = def.beh_suppress_attack;
    n.beh_invulnerable = def.beh_invulnerable;
    n.beh_uncommandable = def.beh_uncommandable;
    n.beh_untargetable = def.beh_untargetable;
    n.beh_life_armor = def.beh_life_armor;
    n.beh_shield_armor = def.beh_shield_armor;
    n.beh_lift = def.beh_lift;
    n.beh_control_transfer = def.beh_control_transfer;
    n.beh_dmg_per_tick = def.beh_dmg_per_tick;
    n.beh_rate_mult = def.beh_rate_mult;
    n.beh_dmg_period = def.beh_dmg_period;
    n.persist_affiliation = def.persist_affiliation;
    n.persist_excludes_structure = def.persist_excludes_structure;
    n.period_count = def.period_count;
    n.spawn_proto = def.spawn_proto;
    n.spawn_count = def.spawn_count;
    n.spawn_range = def.spawn_range;
    n.spawn_snap_resource = def.spawn_snap_resource;
    n.spawn_blocks_ground = def.spawn_blocks_ground;
    n.mineral_cost = def.mineral_cost;
    n.gas_cost = def.gas_cost;
    n.morph_heal_life_frac = def.morph_heal_life_frac;
    n.morph_heal_shield_frac = def.morph_heal_shield_frac;
    n.behavior_tag = def.behavior_tag;
    n.phase_cancel_tag = def.phase_cancel_tag;
    def.root = add_effect(std::move(n));
    abilities_.push_back(def);
    return int32_t(abilities_.size()) - 1;
}

bool World::add_behavior(uint32_t id, const BehaviorInstance& b) {
    if (id >= units_.size()) return false;
    Unit& u = units_[id];
    if (b.kind != 0) {
        int32_t same = 0;
        BehaviorInstance* oldest = nullptr;
        for (BehaviorInstance& e : u.behaviors) {
            if (e.kind != b.kind || e.src != b.src) continue;
            ++same;
            if (!oldest || e.timer < oldest->timer) oldest = &e;
        }
        if (oldest && (b.max_stack == 1 || (b.max_stack > 1 && same >= b.max_stack))) {
            *oldest = b;
            return false;
        }
    }
    u.behaviors.push_back(b);
    return true;
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
    if (apply_persistent_now_) {
        apply_persistent_now_ = false;
        step_persistents(Fixed(), persistents_.size() - 1);
    }
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
    if (n.kind == EffectKind::ApplyBehavior && n.beh_on_caster && ctx.target != ctx.source) {
        EffectContext self = ctx;
        self.target = ctx.source;
        execute_effect(ref, self, depth);
        return;
    }
    if (!n.gates.empty() && ctx.target < units_.size()) {
        const Unit& tgt = units_[ctx.target];
        for (const VitalGate& g : n.gates)
            if (!vital_gate_passes(g, tgt)) return;
    }
    for (const std::uint16_t r : n.validator_roots)
        if (!validator_passes(n.validators, r, ctx)) return;
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
            if (n.search_max_count > 0 && ids.size() > size_t(n.search_max_count)) {
                auto d2 = [&](uint32_t id) {
                    const Fixed dx = units_[id].x - cx, dy = units_[id].y - cy;
                    return dx * dx + dy * dy;
                };
                std::stable_sort(ids.begin(), ids.end(), [&](uint32_t a, uint32_t b) { return d2(a) < d2(b); });
                ids.resize(size_t(n.search_max_count));
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
            const size_t ncase = n.switch_case_roots.size();
            for (size_t i = 0; i < ncase && i < n.children.size(); ++i)
                if (validator_passes(n.switch_ops, n.switch_case_roots[i], ctx, false)) {
                    execute_effect(n.children[i], ctx, depth + 1);
                    return;
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
            issue(who, o, n.issue_queued);
            return;
        }
        case EffectKind::CreateHealer:
            if (start_heal(ctx.source, ctx.target, n.amount, n.heal_drain_factor, ctx.range, n.heal_vital))
                for (EffectRef c : n.children) execute_effect(c, ctx, depth + 1);
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
            p.timer = Fixed();
            p.count = n.period_count + 1;
            apply_persistent_now_ = true;
            p.amount = n.amount;
            p.owner = ctx.origin;
            p.allies = n.persist_allies;
            p.affiliation = n.persist_affiliation;
            p.excludes_structure = n.persist_excludes_structure;
            if (n.persist_follow_source && ctx.source < units_.size()) {
                p.follow = ctx.source;
                p.x = units_[ctx.source].x;
                p.y = units_[ctx.source].y;
            }
            p.periodic = n.periodic;
            p.expire = n.expire;
            p.final_effect = n.final_effect;
            p.src = n.persist_src;
            if (n.initial >= 0) {
                p.timer = n.period;
                p.count = n.period_count;
                apply_persistent_now_ = false;
                execute_effect(n.initial, ctx, depth + 1);
            }
            add_persistent(p);
            return;
        }
        case EffectKind::CreateUnit: {
            if (n.spawn_blocks_ground && n.radius.raw() > 0)
                add_force_field(ctx.tx, ctx.ty, n.radius, n.duration);
            if (n.spawn_proto >= 0 && n.spawn_proto < int32_t(spawn_protos_.size()) &&
                n.spawn_count > 0) {
                PendingSpawn ps{n.spawn_proto, n.spawn_count, ctx.tx, ctx.ty, ctx.origin};
                ps.range = n.spawn_range;
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
            units_[ctx.source].phase_caster_tag = n.behavior_tag;
            units_[ctx.source].phase_cancel_tag = n.phase_cancel_tag;
            if (n.behavior_tag != 0) {
                BehaviorInstance cb;
                cb.kind = kCastBehaviorKind;
                cb.src = n.behavior_tag;
                cb.timer = n.period.raw() > 0 ? n.period : n.duration;
                add_behavior(ctx.source, cb);
            }
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
        case EffectKind::ModifyVitals: {
            const uint32_t who = n.beh_on_caster ? ctx.source : ctx.target;
            if (who >= units_.size() || !units_[who].alive) return;
            Unit& t = units_[who];
            auto apply = [](Fixed& cur, Fixed max, Fixed flat, Fixed frac) {
                cur = cur + flat + max * frac;
                if (cur.raw() < 0) cur = Fixed();
                if (cur > max) cur = max;
            };
            apply(t.hp, t.hp_max, n.vit_life, n.vit_life_frac);
            apply(t.shields, t.shields_max, n.vit_shields, n.vit_shields_frac);
            apply(t.energy, t.energy_max, n.vit_energy, n.vit_energy_frac);
            if (t.hp.raw() <= 0) t.alive = false;
            return;
        }
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
            const Fixed ar = ctx.has_weapon ? ctx.wpn_armor_reduction : Fixed::from_int(1);
            apply_weapon_hit(ctx.target, amt, battr, bamt, ctx.source, ar);
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
            b.max_stack = n.beh_max_stack;
            b.timer = n.duration.raw() > 0 ? n.duration : Fixed::from_raw(1);
            b.speed_mult = n.move_speed_mult;
            b.root = n.beh_root;
            b.stun = n.beh_stun;
            b.suppress_attack = n.beh_suppress_attack;
            b.invulnerable = n.beh_invulnerable;
            b.uncommandable = n.beh_uncommandable;
            b.untargetable = n.beh_untargetable;
            b.life_armor_bonus = n.beh_life_armor;
            b.shield_armor_bonus = n.beh_shield_armor;
            b.lift = n.beh_lift;
            b.dmg_per_tick = n.beh_dmg_per_tick;
            b.dmg_period = n.beh_dmg_period;
            if (n.beh_rate_mult > Fixed::from_int(1) && ctx.target < units_.size()) {
                units_[ctx.target].chrono_timer = n.duration;
                units_[ctx.target].chrono_mult = n.beh_rate_mult;
            }
            b.resp_amount = n.beh_resp_amount;
            b.resp_kinds = n.beh_resp_kinds;
            b.dmg_timer = n.beh_dmg_period;
            b.atk_speed_mult = n.attack_speed_mult.raw() > 0 ? n.attack_speed_mult : Fixed::from_int(1);
            b.fx_periodic = n.periodic;
            b.fx_final = n.final_effect;
            b.fx_period = n.period;
            b.fx_timer = n.period;
            b.fx_caster = ctx.source;
            b.fx_origin = ctx.origin;
            if (n.beh_control_transfer && ctx.target < units_.size() && ctx.source < units_.size()) {
                Unit& ctgt = units_[ctx.target];
                int32_t orig = ctgt.player;
                for (const BehaviorInstance& e : ctgt.behaviors)
                    if (e.ctrl_orig_owner != -2) { orig = e.ctrl_orig_owner; break; }
                b.ctrl_orig_owner = orig;
                b.ctrl_caster = ctx.source;
                ctgt.player = units_[ctx.source].player;
            }
            const bool fresh = add_behavior(ctx.target, b);
            for (int32_t k = 1; k < n.beh_apply_count; ++k) add_behavior(ctx.target, b);
            if (n.behavior_tag != 0 && ctx.target < units_.size() && units_[ctx.target].phase_timer.raw() > 0 &&
                units_[ctx.target].phase_cancel_tag == n.behavior_tag)
                phase_cancel(ctx.target);
            if (n.initial != kNoEffect && ctx.target < units_.size() && units_[ctx.target].alive) {
                EffectContext ic = ctx;
                ic.tx = units_[ctx.target].x;
                ic.ty = units_[ctx.target].y;
                execute_effect(n.initial, ic, depth + 1);
            }
            if (n.beh_lift && ctx.target < units_.size() && units_[ctx.target].alive &&
                !units_[ctx.target].flying) {
                units_[ctx.target].flying = true;
                units_[ctx.target].orders.clear();
            }
            if (fresh && ctx.target < units_.size() && n.attack_speed_mult.raw() > 0 &&
                n.attack_speed_mult.raw() != Fixed::from_int(1).raw())
                units_[ctx.target].atk_speed_mult = units_[ctx.target].atk_speed_mult * n.attack_speed_mult;
            return;
        }
        case EffectKind::DestroyPersistent: {
            const Fixed px = n.destroy_at_source ? ctx.sx : ctx.tx, py = n.destroy_at_source ? ctx.sy : ctx.ty;
            int32_t left = n.destroy_count > 0 ? n.destroy_count : INT32_MAX;
            const size_t count = persistents_.size();
            for (size_t i = 0; i < count && left > 0; ++i) {
                PersistentInstance& p = persistents_[i];
                if (p.count <= 0) continue;
                if (n.destroy_src != 0 && p.src != n.destroy_src) continue;
                if (n.destroy_own_only && p.owner != ctx.origin) continue;
                if (n.radius.raw() > 0) {
                    const Fixed dx = p.x - px, dy = p.y - py;
                    if (dx * dx + dy * dy > n.radius * n.radius) continue;
                }
                p.count = 0;
                --left;
                if (p.final_effect >= 0) {
                    EffectContext ectx;
                    ectx.source = kNoTarget;
                    ectx.origin = p.owner;
                    ectx.sx = ectx.tx = p.x;
                    ectx.sy = ectx.ty = p.y;
                    const EffectRef fi = p.final_effect;
                    execute_effect(fi, ectx, depth + 1);
                }
            }
            return;
        }
        case EffectKind::CancelOrder: {
            const uint32_t who = n.beh_on_caster ? ctx.source : ctx.target;
            if (who >= units_.size()) return;
            Unit& u = units_[who];
            int32_t left = n.remove_count > 0 ? n.remove_count : INT32_MAX;
            auto matches = [&](const Order& o) {
                if (n.cancel_kind == 1) return o.type == OrderType::Attack || o.type == OrderType::AttackMove;
                return o.type != OrderType::Idle;
            };
            std::vector<Order> kept;
            for (size_t k = 0; k < u.orders.size(); ++k) {
                const bool active = k == 0;
                const bool eligible = active ? n.cancel_active : n.cancel_queued;
                if (eligible && left > 0 && matches(u.orders[k])) {
                    --left;
                    if (active) cancel_windup(u);
                    continue;
                }
                kept.push_back(u.orders[k]);
            }
            u.orders.swap(kept);
            return;
        }
        case EffectKind::TransferBehavior: {
            const uint32_t from = n.transfer_from_caster ? ctx.source : ctx.target;
            const uint32_t to = n.transfer_to_caster ? ctx.source : ctx.target;
            if (from >= units_.size() || to >= units_.size() || from == to) return;
            std::vector<BehaviorInstance> moved;
            for (const BehaviorInstance& bi : units_[from].behaviors)
                if (bi.src == n.behavior_tag) moved.push_back(bi);
            if (moved.empty()) return;
            if (!n.transfer_copy) {
                auto& bs = units_[from].behaviors;
                bs.erase(std::remove_if(bs.begin(), bs.end(), [&](const BehaviorInstance& bi) { return bi.src == n.behavior_tag; }), bs.end());
            }
            for (const BehaviorInstance& bi : moved) add_behavior(to, bi);
            return;
        }
        case EffectKind::RemoveBehavior: {
            const uint32_t who = n.beh_on_caster ? ctx.source : ctx.target;
            if (who >= units_.size()) return;
            auto& bs = units_[who].behaviors;
            int32_t left = n.remove_count > 0 ? n.remove_count : INT32_MAX;
            auto take = [&](const BehaviorInstance& bi) {
                if (bi.src != n.behavior_tag || left <= 0) return false;
                --left;
                return true;
            };
            for (const BehaviorInstance& bi : bs) {
                if (!take(bi)) continue;
                queue_behavior_final(units_[who], bi);
                if (bi.atk_speed_mult.raw() > 0 && bi.atk_speed_mult.raw() != Fixed::from_int(1).raw())
                    units_[who].atk_speed_mult = units_[who].atk_speed_mult / bi.atk_speed_mult;
            }
            left = n.remove_count > 0 ? n.remove_count : INT32_MAX;
            bs.erase(std::remove_if(bs.begin(), bs.end(), take), bs.end());
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

bool World::validator_passes(const std::vector<ValidatorOp>& ops, std::uint16_t i, const EffectContext& ctx,
                             bool no_unit_passes) const {
    if (i >= ops.size()) return true;
    const ValidatorOp& v = ops[i];
    bool r = true;
    if (v.kind == ValidatorOp::And) {
        for (const std::uint16_t k : v.kids)
            if (!validator_passes(ops, k, ctx, no_unit_passes)) { r = false; break; }
    } else if (v.kind == ValidatorOp::Or) {
        r = false;
        for (const std::uint16_t k : v.kids)
            if (validator_passes(ops, k, ctx, no_unit_passes)) { r = true; break; }
    } else {
        const std::uint32_t who = v.on_caster ? ctx.source : ctx.target;
        if (who >= units_.size()) return no_unit_passes;
        const Unit& u = units_[who];
        switch (v.kind) {
            case ValidatorOp::Vital: r = vital_gate_passes(v.vital, u); break;
            case ValidatorOp::UnitType: r = u.catalog_id == v.unit_type; break;
            case ValidatorOp::NoBehavior:
                for (const BehaviorInstance& b : u.behaviors)
                    if (b.src == v.behavior) { r = false; break; }
                break;
            case ValidatorOp::Filters: {
                const std::uint8_t plane = u.flying ? 2 : 1;
                std::uint8_t rel = 8;
                if (u.id == ctx.source) rel = 1;
                else if (u.player == ctx.origin) rel = 2;
                else if (u.player < 0) rel = 16;
                else if (ctx.origin >= 0 && are_allied(u.player, ctx.origin)) rel = 4;
                r = (u.attributes & v.req_attr) == v.req_attr && (u.attributes & v.exc_attr) == 0 &&
                    (v.req_plane == 0 || (v.req_plane & plane) != 0) && (v.exc_plane & plane) == 0 &&
                    (v.exc_rel & rel) == 0 && !(v.exc_invulnerable && is_invulnerable(u));
                break;
            }
            default: break;
        }
    }
    return v.negate ? !r : r;
}

void World::apply_spell_damage(uint32_t target, Fixed dmg, uint32_t caster, Fixed armor_reduction) {
    if (target >= units_.size()) return;
    Unit& t = units_[target];
    if (!t.alive) return;
    if (is_invulnerable(t)) return;
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
    if (is_invulnerable(t)) return;
    Fixed resp;
    for (const BehaviorInstance& bi : t.behaviors)
        if (bi.timer.raw() > 0 && (bi.resp_kinds & hit_kind_) && bi.resp_amount.raw() < 0 && -bi.resp_amount > resp)
            resp = -bi.resp_amount;
    Defender d{t.hp, t.shields, t.armor + behavior_armor(t, false), t.damage_reduction + t.aura_reduction + resp};
    d.shield_armor = t.shield_armor + behavior_armor(t, true);
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
    if ((t.hp + t.shields) >= vitals_before || attacker >= units_.size()) return;
    Unit& a = units_[attacker];
    if (!a.alive || a.player < 0 || are_allied(a.player, t.player)) return;
    last_progress_tick_ = tick_;
    if (attack_reveal_time_.raw() > 0) {
        a.reveal_to = t.player;
        a.reveal_timer = attack_reveal_time_;
    }
    if (help_radius_.raw() > 0 && t.help_cd.raw() <= 0) {
        t.help_cd = help_period_;
        const Fixed r2 = help_radius_ * help_radius_;
        for (Unit& o : units_) {
            if (!o.alive || o.id == t.id || o.id == attacker || !are_allied(o.player, t.player)) continue;
            if (!o.orders.empty() || o.target != kNoTarget || o.is_worker || o.flees_when_attacked) continue;
            if (o.wpn_range.raw() <= 0 || o.loaded) continue;
            const Fixed dx = o.x - t.x, dy = o.y - t.y;
            if (dx * dx + dy * dy > r2) continue;
            issue(o.id, Order{OrderType::Attack, Fixed(), Fixed(), attacker}, false,  false);
        }
    }
}

void World::apply_weapon_fire(uint32_t attacker, uint32_t target, Fixed w_dmg, uint32_t w_battr,
                              Fixed w_bamt, int hits, Fixed w_ar, std::uint8_t w_slot) {
    if (attacker >= units_.size() || target >= units_.size()) return;
    Unit& u = units_[attacker];
    if (u.deals_no_damage) return;
    struct KindScope {
        std::uint8_t& k; std::uint8_t saved;
        KindScope(std::uint8_t& ref, std::uint8_t v) : k(ref), saved(ref) { k = v; }
        ~KindScope() { k = saved; }
    } kind_scope(hit_kind_, w_slot == 1 ? u.wpn2_kind : u.wpn_kind);
    Unit& tgt = units_[target];
    const uint32_t atk = target;
    const Fixed primary_dmg =
        u.behaviors.empty() ? w_dmg : w_dmg + behavior_weapon_bonus(u, tgt.attributes);
    if (u.wpn_effect >= 0) {
        const int n = (multi_hit_armor_ && hits > 1) ? hits : 1;
        const Fixed per = primary_dmg / Fixed::from_int(n);
        Fixed dacc;
        for (int k = 0; k < n; ++k) {
            const bool last = (k == n - 1);
            EffectContext ctx;
            ctx.source = u.id;
            ctx.target = tgt.id;
            ctx.origin = u.player;
            ctx.has_weapon = true;
            ctx.wpn_amount = last ? (primary_dmg - dacc) : per;
            ctx.wpn_bonus_attr = w_battr;
            ctx.wpn_bonus_amount = w_bamt;
            ctx.wpn_armor_reduction = w_ar;
            dacc = dacc + per;
            execute_effect(u.wpn_effect, ctx);
        }
    } else if (multi_hit_armor_ && hits > 1) {
        const Fixed per = primary_dmg / Fixed::from_int(hits);
        Fixed dacc;
        for (int k = 0; k < hits; ++k) {
            const bool last = (k == hits - 1);
            const Fixed h = last ? (primary_dmg - dacc) : per;
            dacc = dacc + per;
            apply_weapon_hit(tgt.id, h, w_battr, w_bamt, u.id, w_ar);
        }
    } else {
        apply_weapon_hit(tgt.id, primary_dmg, w_battr, w_bamt, u.id, w_ar);
    }
    if (u.attack_spawn_proto >= 0 && u.attack_spawn_count > 0 &&
        u.attack_spawn_proto < int32_t(spawn_protos_.size()))
        pending_spawns_.push_back(
            {u.attack_spawn_proto, u.attack_spawn_count, tgt.x, tgt.y, u.player, kNoTarget, false});
    const Fixed w_btot = w_bamt * Fixed::from_int(hits);
    const bool suppress_splash = u.no_splash_radius.raw() > 0 && tgt.radius >= u.no_splash_radius;
    if (u.splash_outer.raw() > 0 && !suppress_splash) {
        const Fixed tx = tgt.x, ty = tgt.y;
        for (Unit& o : units_) {
            if (!o.alive || o.id == atk || !splash_hits(u, o)) continue;
            if (o.loaded || o.burrowed) continue;
            const Fixed dx = o.x - tx, dy = o.y - ty;
            const Fixed d2 = dx * dx + dy * dy;
            const Fixed slack = u.splash_offset_by_radius ? o.radius : Fixed();
            const Fixed ri = u.splash_inner + slack, rm = u.splash_mid + slack, ro = u.splash_outer + slack;
            Fixed frac;
            if (d2 <= ri * ri) frac = u.splash_inner_frac;
            else if (d2 <= rm * rm) frac = u.splash_mid_frac;
            else if (d2 <= ro * ro) frac = u.splash_outer_frac;
            else continue;
            { const std::uint8_t sk = hit_kind_; hit_kind_ = kDamageKindSplash; apply_weapon_hit(o.id, w_dmg * frac, w_battr, w_btot * frac, u.id, w_ar); hit_kind_ = sk; }
        }
    }
    if (u.splash_line_count > 0) {
        const Fixed tx = tgt.x, ty = tgt.y;
        for (Unit& o : units_) {
            if (!o.alive || o.id == atk || !splash_hits(u, o)) continue;
            if (o.loaded || o.burrowed || o.flying) continue;
            if (in_line_splash(u.x, u.y, tx, ty, u.splash_line_first, u.splash_line_step,
                               u.splash_line_count, u.splash_line_radius, o.x, o.y,
                               u.splash_line_from_source))
                { const std::uint8_t sk = hit_kind_; hit_kind_ = kDamageKindSplash; apply_weapon_hit(o.id, w_dmg, w_battr, w_btot, u.id, w_ar); hit_kind_ = sk; }
        }
    }
    if (u.arc_radius[0].raw() > 0 || u.arc_radius[1].raw() > 0) {
        const Fixed tx = tgt.x, ty = tgt.y;
        for (Unit& o : units_) {
            if (!o.alive || o.id == atk || !splash_hits(u, o)) continue;
            if (o.loaded || o.burrowed || o.flying) continue;
            Fixed frac;
            for (int k = 0; k < 2; ++k)
                if (u.arc_radius[k].raw() > 0 && u.arc_frac[k].raw() > 0 &&
                    in_arc(u.x, u.y, tx, ty, u.arc_radius[k], u.arc_cos[k], o.x, o.y))
                    frac = frac + u.arc_frac[k];
            if (frac.raw() > 0)
                { const std::uint8_t sk = hit_kind_; hit_kind_ = kDamageKindSplash; apply_weapon_hit(o.id, w_dmg * frac, w_battr, w_btot * frac, u.id, w_ar); hit_kind_ = sk; }
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

void World::step_landing(Fixed dt) {
    for (Unit& u : units_) {
        if (!u.land_pending) continue;
        if (!u.alive || !u.flying) {
            u.land_pending = false;
            continue;
        }
        if (u.land_timer.raw() < 0) {
            if (!u.orders.empty()) {
                const Order& o = u.orders.front();
                if (o.type != OrderType::Move || o.tx != u.land_x || o.ty != u.land_y) u.land_pending = false;
                continue;
            }
            u.land_timer = u.land_time;
            u.lift_lock = u.land_time;
        }
        u.land_timer = u.land_timer - dt;
        if (u.land_timer.raw() > 0) continue;
        u.land_pending = false;
        land(u.id, u.land_x, u.land_y);
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
                queue_projectile(i, u.x, u.y, t, ps, u.windup_dmg, u.windup_battr, u.windup_bamt, u.windup_ar,
                                 u.windup_hits, u.windup_slot);
            else
                apply_weapon_fire(i, t, u.windup_dmg, u.windup_battr, u.windup_bamt, u.windup_hits,
                                  u.windup_ar, u.windup_slot);
        }
    }
}

void World::queue_projectile(std::uint32_t attacker, Fixed px, Fixed py, std::uint32_t target, Fixed speed,
                             Fixed dmg, std::uint32_t battr, Fixed bamt, Fixed ar, int hits,
                             std::uint8_t slot) {
    Fixed flife = Fixed::from_int(1);
    if (target < units_.size()) {
        const Fixed fdx = units_[target].x - px, fdy = units_[target].y - py;
        flife = isqrt(fdx * fdx + fdy * fdy) * Fixed::from_int(4) / speed + Fixed::from_int(1);
    }
    pending_hits_.push_back({attacker, target, px, py, speed, dmg, battr, bamt, flife, ar, hits, slot});
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
            apply_weapon_fire(p.attacker, p.target, p.dmg, p.battr, p.bamt, p.hits, p.ar, p.slot);
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
    if (killer.kills < 0x7fffffff) ++killer.kills;
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
    if (producer >= units_.size()) return false;
    if (units_[producer].constructing || units_[producer].addon_building != 0) return false;
    if (!tech_requirements_met(units_[producer].player, proto.catalog_id)) return false;
    if (train_gate_ && !train_gate_(producer, proto.catalog_id)) return false;
    if (!techlab_gated_.empty() && techlab_gated_.count(proto.catalog_id) &&
        units_[producer].addon != kAddonTechLab)
        return false;
    const bool larva_morph = (units_[producer].train_morphs_self || units_[producer].spawner_proto >= 0) &&
                             proto.speed.raw() > 0 && !proto.hall_trained;
    if (const int32_t depth = queue_depth(producer); depth > 0 && !larva_morph)
        if (queue_count(producer) >= depth) return false;
    if (train_affordability_) {
        const int32_t pl_gate = units_[producer].player;
        const Player& pg = player(pl_gate);
        const int32_t n_gate = count > 1 ? count : 1;
        if (pg.minerals < mineral_cost * n_gate) { refuse(pl_gate, CmdError::NotEnoughMinerals); return false; }
        if (pg.gas < gas_cost * n_gate) { refuse(pl_gate, CmdError::NotEnoughVespene); return false; }
        if (!supply_room(pl_gate, proto.supply_uses, n_gate)) {
            refuse(pl_gate, CmdError::NotEnoughFood);
            return false;
        }
    }
    uint32_t consumed_larva = kNoTarget;
    uint32_t egg_id = kNoTarget;
    const bool self_morph = units_[producer].train_morphs_self;
    if (larva_morph) {
        uint32_t larva = kNoTarget;
        if (self_morph) {
            larva = producer;
        } else {
            for (uint32_t i = 0; i < units_.size(); ++i)
                if (units_[i].alive && units_[i].spawn_parent == producer) { larva = i; break; }
        }
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
    Pending pd{producer, u, build_time, build_time};
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
    if (reporting()) {
        Command cmd;
        cmd.kind = CommandKind::Train;
        cmd.proto = proto;
        cmd.mineral_cost = mineral_cost;
        cmd.gas_cost = gas_cost;
        cmd.build_time = build_time;
        cmd.count = count;
        report_command(producer, cmd);
    }
    return true;
}

int World::queued_count(uint32_t producer) const {
    int n = 0;
    for (const Pending& pd : pending_)
        if (pd.producer == producer) ++n;
    return n;
}

bool World::cancel_train(uint32_t producer) {
    return cancel_train_slot(producer, queued_count(producer) - 1);
}

bool World::cancel_train_slot(uint32_t producer, int slot) {
    if (slot < 0) return false;
    int idx = -1, seen = 0;
    for (std::size_t i = 0; i < pending_.size(); ++i)
        if (pending_[i].producer == producer && seen++ == slot) { idx = int(i); break; }
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

bool World::cancel_construction(uint32_t building, int32_t mineral_cost, int32_t gas_cost, Fixed refund) {
    if (building >= units_.size()) return false;
    Unit& u = units_[building];
    if (!u.alive || !u.constructing) return false;
    Player& pp = player(u.player);
    pp.minerals += int32_t((int64_t(mineral_cost) * refund.raw()) / Fixed::from_int(1).raw());
    pp.gas += int32_t((int64_t(gas_cost) * refund.raw()) / Fixed::from_int(1).raw());
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
    const int32_t bp = units_[builder].player;
    if (!creep_placement_ok(proto, x, y)) { refuse(bp, CmdError::CantBuildTooFarFromCreepSource); return false; }
    if (!resource_halo_ok(proto, x, y)) { refuse(bp, CmdError::CantBuildTooCloseToResources); return false; }
    if (placement_.configured()) {
        int cx = 0, cy = 0;
        placement_.to_cell(x, y, cx, cy);
        int minx = 0, maxx = 0, miny = 0, maxy = 0;
        for (const std::pair<int16_t, int16_t>& o : proto.footprint_offsets) {
            if (o.first < minx) minx = o.first;
            if (o.first > maxx) maxx = o.first;
            if (o.second < miny) miny = o.second;
            if (o.second > maxy) maxy = o.second;
        }
        const int fx = cx + minx, fy = cy + miny;
        const int fw = maxx - minx + 1, fh = maxy - miny + 1;
        if (!placement_.can_place(fx, fy, fw, fh,  proto.requires_vespene))
            return false;
        if (!footprint_clear_of_blockers(*this, placement_, fx, fy, fw, fh, builder,
                                         proto.requires_vespene))
            return false;
    }
    if (proto.needs_power && !powered_at(x, y, units_[builder].player)) {
        refuse(bp, CmdError::CantBuildTooFarFromBuildPowerSource);
        return false;
    }
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
    const bool was_internal = in_step_;
    in_step_ = true;
    issue(builder, o, false);
    in_step_ = was_internal;
    worker_builds_.push_back(WorkerBuild{builder, x, y, proto, build_time, arrive, verb});
    if (reporting()) {
        Command cmd;
        cmd.kind = CommandKind::WorkerBuild;
        cmd.proto = proto;
        cmd.x = x;
        cmd.y = y;
        cmd.mineral_cost = mineral_cost;
        cmd.gas_cost = gas_cost;
        cmd.build_time = build_time;
        cmd.arrive = arrive;
        cmd.verb = verb;
        report_command(builder, cmd);
    }
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
    if (train_gate_ && !train_gate_(producer, proto.catalog_id)) return false;
    if (const int32_t depth = queue_depth(producer); depth > 0)
        if (queue_count(producer) >= depth) return false;
    int32_t pl = units_[producer].player;
    Player& pp = player(pl);
    const int32_t n = count > 1 ? count : 1;
    pp.minerals -= mineral_cost * n;
    pp.gas -= gas_cost * n;
    Unit u = proto;
    u.player = pl;
    Pending pd{producer, u, build_time, build_time};
    pd.has_pos = true;
    pd.px = x;
    pd.py = y;
    pd.count = n;
    pd.mineral_cost = mineral_cost * n;
    pd.gas_cost = gas_cost * n;
    pending_.push_back(std::move(pd));
    if (reporting()) {
        Command cmd;
        cmd.kind = CommandKind::TrainAt;
        cmd.proto = proto;
        cmd.mineral_cost = mineral_cost;
        cmd.gas_cost = gas_cost;
        cmd.build_time = build_time;
        cmd.x = x;
        cmd.y = y;
        cmd.count = count;
        report_command(producer, cmd);
    }
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
    }

    if (u.build_time.raw() <= 0) {
        u.build_progress = Fixed::from_int(1);
        u.hp = u.hp_max;
        u.constructing = false;
        arm_spawner_on_completion(u);
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
        arm_spawner_on_completion(u);
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
    power_sources_.clear();
    for (const Unit& s : units_)
        if (s.alive && !s.constructing && s.power_radius.raw() > 0) power_sources_.push_back(s.id);
    for (Unit& u : units_) {
        if (!u.alive || !u.needs_power) continue;
        bool on = false;
        for (const uint32_t sid : power_sources_) {
            const Unit& s = units_[sid];
            if (!are_allied(s.player, u.player)) continue;
            const Fixed dx = u.x - s.x, dy = u.y - s.y;
            if (dx * dx + dy * dy <= s.power_radius * s.power_radius) { on = true; break; }
        }
        u.powered = on;
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

Fixed World::reload_after_shot(const Unit& u, Fixed period, Fixed rd_min, Fixed rd_max) {
    Fixed cd = (u.atk_speed_mult.raw() == Fixed::from_int(1).raw() || u.atk_speed_mult.raw() <= 0)
                   ? period
                   : period / u.atk_speed_mult;
    if (rd_min.raw() != 0 || rd_max.raw() != 0) {
        const Fixed u01 = Fixed::from_ratio(int(rng_.below(4096)), 4096);
        cd = cd + rd_min + (rd_max - rd_min) * u01;
        if (cd.raw() < 0) cd = Fixed();
    }
    return cd;
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
            apply_weapon_fire(uint32_t(pi), best, p.wpn_damage, p.wpn_bonus_attr, p.wpn_bonus_amount, p.wpn_hits,
                              p.wpn_armor_reduction, 0);
            p.cooldown = reload_after_shot(p, p.wpn_period, p.wpn_random_delay_min, p.wpn_random_delay_max);
        }
    }
}

void World::step_persistents(Fixed dt, size_t only) {
    for (size_t i = (only == kAllPersistents ? 0 : only); i < persistents_.size();) {
        PersistentInstance& p = persistents_[i];
        if (p.count <= 0) {
            persistents_.erase(persistents_.begin() + i);
            continue;
        }
        p.timer = p.timer - dt;
        if (p.timer.raw() <= 0) {
            p.timer = p.timer + p.period;
            if (p.follow < units_.size()) {
                if (!units_[p.follow].alive) {
                    p.count = 0;
                    if (p.final_effect >= 0) {
                        EffectContext ectx;
                        ectx.source = kNoTarget;
                        ectx.origin = p.owner;
                        ectx.sx = ectx.tx = p.x;
                        ectx.sy = ectx.ty = p.y;
                        const EffectRef fi = p.final_effect;
                        execute_effect(fi, ectx);
                    }
                    if (only != kAllPersistents) break;
                    ++i;
                    continue;
                }
                p.x = units_[p.follow].x;
                p.y = units_[p.follow].y;
            }
            Fixed cx = p.x, cy = p.y;
            if (!p.offsets.empty()) {
                const size_t k = size_t(p.applied) < p.offsets.size() ? size_t(p.applied)
                                                                      : p.offsets.size() - 1;
                const Fixed ox = p.offsets[k].first, oy = p.offsets[k].second;
                cx = p.x + ox * p.face_x - oy * p.face_y;
                cy = p.y + ox * p.face_y + oy * p.face_x;
            }
            for (Unit& u : units_) {
                if (!u.alive) continue;
                if (p.affiliation != 0) {
                    std::uint8_t rel = 0;
                    if (u.player == p.owner) rel = 2;
                    else if (u.player < 0) rel = 16;
                    else if (are_allied(u.player, p.owner)) rel = 4;
                    else rel = 8;
                    if ((p.affiliation & rel) == 0) continue;
                    if (p.excludes_structure && (u.attributes & kAttrStructure)) continue;
                } else if (p.allies ? !are_allied(u.player, p.owner) : u.player == p.owner) {
                    continue;
                }
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
            if (p.count <= 0 && (p.expire >= 0 || p.final_effect >= 0)) {
                EffectContext ectx;
                ectx.source = kNoTarget;
                ectx.origin = p.owner;
                ectx.sx = ectx.tx = p.x;
                ectx.sy = ectx.ty = p.y;
                const EffectRef ex = p.expire, fi = p.final_effect;
                if (ex >= 0) execute_effect(ex, ectx);
                if (fi >= 0) execute_effect(fi, ectx);
            }
        }
        if (only != kAllPersistents) break;
        ++i;
    }
}

bool World::start_heal(std::uint32_t caster, std::uint32_t target, Fixed heal_rate, Fixed drain_factor,
                       Fixed range, HealVital vital) {
    if (caster >= units_.size() || target >= units_.size() || caster == target) return false;
    const Unit& c = units_[caster];
    const Unit& t = units_[target];
    const bool full = vital == HealVital::Shields ? (t.shields_max.raw() <= 0 || t.shields >= t.shields_max)
                                                  : t.hp >= t.hp_max;
    if (!c.alive || !t.alive || full) return false;
    for (HealerInstance& h : healers_)
        if (h.caster == caster && h.target == target) {
            h.vital = vital;
            h.heal_rate = heal_rate;
            h.drain_factor = drain_factor;
            h.range = range;
            return true;
        }
    healers_.push_back(HealerInstance{caster, target, heal_rate, drain_factor, range, vital});
    return true;
}

void World::step_healers(Fixed dt) {
    for (size_t i = 0; i < healers_.size();) {
        HealerInstance& h = healers_[i];
        bool retire = h.caster >= units_.size() || h.target >= units_.size();
        if (!retire) {
            Unit& c = units_[h.caster];
            Unit& t = units_[h.target];
            const bool shields = h.vital == HealVital::Shields;
            if (!c.alive || !t.alive || (shields ? t.shields >= t.shields_max : t.hp >= t.hp_max))
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
                        Fixed& v = shields ? t.shields : t.hp;
                        const Fixed vmax = shields ? t.shields_max : t.hp_max;
                        Fixed nh = v + heal;
                        if (nh > vmax) nh = vmax;
                        v = nh;
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
    prod_rank_.resize(pending_.size());
    prod_done_.assign(units_.size(), 0);
    for (size_t k = 0; k < pending_.size(); ++k) {
        const uint32_t pk = pending_[k].producer;
        prod_rank_[k] = pk < prod_done_.size() ? prod_done_[pk]++ : 0;
    }
    prod_done_.assign(units_.size(), 0);
    auto drop = [&](size_t at) {
        const uint32_t pk = pending_[at].producer;
        if (pk < prod_done_.size()) ++prod_done_[pk];
        pending_.erase(pending_.begin() + at);
        prod_rank_.erase(prod_rank_.begin() + at);
    };
    for (size_t i = 0; i < pending_.size();) {
        Pending& pd = pending_[i];
        if (pd.egg != kNoTarget && (pd.egg >= units_.size() || !units_[pd.egg].alive)) {
            drop(i);
            continue;
        }
        if (pd.producer < units_.size()) {
            const Unit& pr = units_[pd.producer];
            if (pr.needs_power && pr.power_managed && !pr.powered) { ++i; continue; }
        }
        if (pd.producer < units_.size() && pd.egg == kNoTarget) {
            const int32_t slots = units_[pd.producer].train_slots;
            if (slots > 0) {
                const int ahead = prod_rank_[i] - (pd.producer < prod_done_.size() ? prod_done_[pd.producer] : 0);
                if (ahead >= slots) { ++i; continue; }
            }
        }
        Fixed adv = dt;
        if (pd.producer < units_.size() && units_[pd.producer].chrono_timer.raw() > 0)
            adv = adv * units_[pd.producer].chrono_mult;
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
            drop(i);
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
                if (!u.carry_override && m.return_delay.raw() > 0) u.return_delay = m.return_delay;
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
        {
            if (dropoffs_tick_ != tick_ || dropoffs_n_ != units_.size()) {
                dropoffs_.clear();
                for (const Unit& d : units_)
                    if ((d.drop_off_ground || d.drop_off_air) && !d.catalog_id.empty()) dropoffs_.push_back(d.id);
                dropoffs_tick_ = tick_;
                dropoffs_n_ = units_.size();
            }
            std::uint32_t best = kNoTarget;
            Fixed bd;
            for (const std::uint32_t di : dropoffs_) {
                const Unit& d = units_[di];
                if (!d.alive || d.player != u.player || d.id == u.id || d.catalog_id.empty() || d.constructing ||
                    d.speed.raw() != 0 || d.resource_left > 0 || d.is_worker)
                    continue;
                if (!(d.flying ? d.drop_off_air : d.drop_off_ground)) continue;
                const Fixed ddx = d.x - u.x, ddy = d.y - u.y;
                const Fixed dd = ddx * ddx + ddy * ddy;
                if (best == kNoTarget || dd < bd) { best = d.id; bd = dd; }
            }
            if (best != kNoTarget) u.home_depot = best;
        }
        if (u.home_depot >= units_.size() || !units_[u.home_depot].alive) return;
        Unit& d = units_[u.home_depot];
        if (!(d.flying ? d.drop_off_air : d.drop_off_ground)) return;
        Fixed reach = u.radius + d.radius + Fixed::from_ratio(1, 4);
        if (dist2(u, d) <= reach * reach) {
            if (u.gathers_gas) player(u.player).gas += u.carrying;
            else player(u.player).minerals += u.carrying;
            u.carrying = 0;
            last_progress_tick_ = tick_;
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
                if (active >= 1) {
                    auto assigned_to = [&](const Unit& o) {
                        int n = 0;
                        for (const Unit& w : units_)
                            if (w.alive && w.id != u.id && !w.saturation_exempt && w.gather_target == o.id &&
                                !w.orders.empty() && w.orders.front().type == OrderType::Gather)
                                ++n;
                        return n;
                    };
                    auto assign_cap = [](const Unit& o) { return o.is_gas ? o.miners_max : o.miners_max + 1; };
                    if (u.acquire_radius.raw() > 0) {
                        std::uint32_t best = kNoTarget;
                        Fixed best_d2;
                        const Fixed rr = u.acquire_radius;
                        const bool under_max_here = assigned_to(m) < assign_cap(m);
                        for (int pass = 0; pass < 2 && best == kNoTarget; ++pass) {
                          if (pass == 1 && under_max_here) break;
                          for (const Unit& o : units_) {
                            if (!o.alive || o.resource_left <= 0 || o.id == u.gather_target) continue;
                            if (o.is_gas != m.is_gas) continue;
                            const Fixed dx = o.x - m.x, dy = o.y - m.y;
                            const Fixed d2 = dx * dx + dy * dy;
                            if (d2 > rr * rr) continue;
                            if (o.miners_max > 0) {
                                const int assigned = assigned_to(o);
                                if (assigned >= (pass == 0 ? o.miners_max : assign_cap(o))) continue;
                            }
                            if (best == kNoTarget || d2 < best_d2) { best = o.id; best_d2 = d2; }
                          }
                        }
                        if (best != kNoTarget) {
                            u.gather_target = best;
                            return;
                        }
                    }
                    return;
                }
            }
            u.mine_cd = m.harvest_time.raw() > 0
                            ? m.harvest_time
                            : (u.mine_time.raw() > 0 ? u.mine_time
                                                     : Fixed::parse(m.is_gas ? "1.981" : "2.786"));
            if (u.mine_time_mult.raw() > 0) u.mine_cd = u.mine_cd * u.mine_time_mult;
        } else {
            const Fixed bx = u.x, by = u.y;
            move_unit_toward(u, m.x, m.y, move_step(u, dt));
            if (u.x == bx && u.y == by) {
                if (++u.gather_stall_ticks >= kTicksPerSecond && u.acquire_radius.raw() > 0) {
                    u.gather_stall_ticks = 0;
                    uint32_t best = kNoTarget;
                    Fixed best_d2;
                    const Fixed rr = u.acquire_radius;
                    for (const Unit& o : units_) {
                        if (!o.alive || o.resource_left <= 0 || o.id == u.gather_target || o.is_gas != m.is_gas) continue;
                        const Fixed dx = o.x - u.x, dy = o.y - u.y, d2 = dx * dx + dy * dy;
                        if (d2 > rr * rr) continue;
                        if (best == kNoTarget || d2 < best_d2) { best = o.id; best_d2 = d2; }
                    }
                    if (best != kNoTarget) u.gather_target = best;
                }
            } else {
                u.gather_stall_ticks = 0;
            }
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
                ic.ever_ordered = true;
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

void World::begin_research(int32_t player, const std::string& id, Fixed delay) {
    if (has_upgrade(player, id)) return;
    for (const PendingResearch& p : research_pending_)
        if (p.player == player && p.id == id) return;
    research_pending_.push_back(PendingResearch{player, id, delay, delay});
    if (reporting()) {
        Command cmd;
        cmd.kind = CommandKind::Research;
        cmd.player = player;
        cmd.research_id = id;
        cmd.build_time = delay;
        report_command(kNoTarget, cmd);
    }
}

void World::grant_upgrade(int32_t player, const std::string& id) {
    if (!has_upgrade(player, id)) researched_[player].push_back(id);
}

const std::vector<std::string>& World::researched(int32_t player) const {
    static const std::vector<std::string> kNone;
    auto it = researched_.find(player);
    return it == researched_.end() ? kNone : it->second;
}

bool World::has_upgrade(int32_t player, const std::string& id) const {
    const std::vector<std::string>& v = researched(player);
    return std::find(v.begin(), v.end(), id) != v.end();
}

bool World::research_pending(int32_t player, const std::string& id) const {
    for (const PendingResearch& p : research_pending_)
        if (p.player == player && p.id == id) return true;
    return false;
}

std::vector<std::pair<int32_t, std::string>> World::take_completed_research() {
    std::vector<std::pair<int32_t, std::string>> out;
    out.swap(research_done_);
    return out;
}

void World::step_research(Fixed dt) {
    if (research_pending_.empty()) return;
    for (std::size_t i = 0; i < research_pending_.size();) {
        research_pending_[i].timer = research_pending_[i].timer - dt;
        if (research_pending_[i].timer.raw() <= 0) {
            if (!has_upgrade(research_pending_[i].player, research_pending_[i].id)) {
                research_done_.emplace_back(research_pending_[i].player, research_pending_[i].id);
                grant_upgrade(research_pending_[i].player, research_pending_[i].id);
            }
            research_pending_.erase(research_pending_.begin() + std::ptrdiff_t(i));
        } else {
            ++i;
        }
    }
}

void World::step() {
    const Fixed dt = World::dt();
    in_step_ = true;
    for (Unit& u : units_) u.loco_moved = Fixed();
    update_conjoined();
    return_dead_carry();
    update_destructibles();
    release_dead_creep_sources();
    step_research(dt);
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
    step_landing(dt);
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
                if (u.burn_timer >= u.burn_period) {
                    u.burn_timer = u.burn_timer - u.burn_period;
                    u.hp = u.hp - u.burn_amount;
                    if (u.hp.raw() <= 0) { u.hp = Fixed(); u.alive = false; continue; }
                }
            } else {
                u.burn_timer = Fixed();
            }
        }
        if (u.phase_timer.raw() > 0) {
            u.phase_timer = u.phase_timer - dt;
            if (u.phase_timer.raw() <= 0) {
                if (u.phase_shade < units_.size() && units_[u.phase_shade].alive) {
                    u.x = units_[u.phase_shade].x;
                    u.y = units_[u.phase_shade].y;
                    units_[u.phase_shade].alive = false;
                }
                end_phase(u);
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
        if (u.toggle_wpn_on && u.toggle_wpn_drain.raw() > 0) {
            u.energy = u.energy - u.toggle_wpn_drain * dt;
            if (u.energy.raw() <= 0) {
                u.energy = Fixed();
                u.swap_weapon_block(u.toggle_wpn);
                u.toggle_wpn_on = false;
            }
        }

        if (u.salvage_timer.raw() > 0) {
            u.salvage_timer = u.salvage_timer - dt;
            if (u.salvage_timer.raw() <= 0) {
                u.salvage_timer = Fixed();
                if (u.player >= 0) {
                    Player& p = player(u.player);
                    p.minerals += u.salvage_minerals;
                    p.gas += u.salvage_gas;
                }
                u.alive = false;
                continue;
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
                std::vector<BehaviorInstance> keep_perm;
                for (const BehaviorInstance& b : u.behaviors)
                    if (b.permanent) keep_perm.push_back(b);
                const uint32_t finish_beh = u.morph_finish_beh;
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
                u.behaviors.insert(u.behaviors.end(), keep_perm.begin(), keep_perm.end());
                if (finish_beh != 0) {
                    bool already = false;
                    for (const BehaviorInstance& b : u.behaviors)
                        if (b.permanent && b.src == finish_beh) already = true;
                    if (!already) {
                        BehaviorInstance perm;
                        perm.src = finish_beh;
                        perm.permanent = true;
                        u.behaviors.push_back(perm);
                    }
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
                const Fixed step = u.burrow_speed.raw() > 0 ? u.burrow_speed * dt : move_step(u, dt);
                if (move_unit_toward(u, o.tx, o.ty, step))
                    u.orders.erase(u.orders.begin());
                continue;
            }
            if (!u.can_burrow_attack) continue;
        }

        if (u.constructing) {
            step_construction(u, dt);
            continue;
        }

        if (u.addon_body != kNoTarget && (u.addon_body >= units_.size() || !units_[u.addon_body].alive || !u.alive)) {
            if (u.addon_body < units_.size()) units_[u.addon_body].addon_host = kNoTarget;
            u.addon_body = kNoTarget;
            u.addon = kAddonNone;
            u.train_slots = 1;
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
            int natural = 0, total = 0;
            brood_counts(u.id, natural, total);
            const bool under_hard = u.spawner_hard_cap <= 0 || total < u.spawner_hard_cap;
            if (natural < u.spawner_cap && under_hard) {
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
                if (!b.permanent) b.timer = b.timer - dt;
                if (b.ctrl_orig_owner != -2) {
                    if (b.ctrl_caster >= units_.size() || !units_[b.ctrl_caster].alive) b.timer = Fixed();
                    if (b.timer.raw() <= 0 && u.alive) u.player = b.ctrl_orig_owner;
                }
                if (b.lift && b.timer.raw() <= 0 && u.alive) { u.flying = false; u.air_expand = 0; }
                if (!b.permanent && b.timer.raw() <= 0 && b.atk_speed_mult.raw() > 0 &&
                    b.atk_speed_mult.raw() != Fixed::from_int(1).raw()) {
                    u.atk_speed_mult = u.atk_speed_mult / b.atk_speed_mult;
                    b.atk_speed_mult = Fixed::from_int(1);
                }
                if (b.dmg_per_tick.raw() > 0 && b.dmg_period.raw() > 0 && u.alive) {
                    b.dmg_timer = b.dmg_timer - dt;
                    if (b.dmg_timer.raw() <= 0) {
                        b.dmg_timer = b.dmg_period;
                        apply_spell_damage(u.id, b.dmg_per_tick, kNoTarget, Fixed::from_int(1));
                    }
                }
                if (b.fx_periodic != kNoEffect && b.fx_period.raw() > 0 && u.alive && b.timer.raw() > 0) {
                    b.fx_timer = b.fx_timer - dt;
                    if (b.fx_timer.raw() <= 0) {
                        b.fx_timer = b.fx_timer + b.fx_period;
                        behavior_fx_.push_back({b.fx_periodic, u.id, b.fx_caster, b.fx_origin});
                    }
                }
                if (!b.permanent && b.timer.raw() <= 0) queue_behavior_final(u, b);
            }
            u.behaviors.erase(
                std::remove_if(u.behaviors.begin(), u.behaviors.end(),
                               [](const BehaviorInstance& b) {
                                   return !b.permanent && b.timer.raw() <= 0;
                               }),
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
            const bool own_target_ok = u.gather_target < units_.size() &&
                                       units_[u.gather_target].alive &&
                                       units_[u.gather_target].resource_left > 0;
            if (!own_target_ok && go.target < units_.size() && units_[go.target].resource_left > 0)
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
            const Fixed reach = u.radius + t.radius + t.load_range;
            if (dist2(u, t) <= reach * reach) {
                load_unit(tid, u.id);
                u.orders.erase(u.orders.begin());
            } else {
                move_unit_toward(u, t.x, t.y, move_step(u, dt));
            }
            continue;
        }

        if (!u.orders.empty() && u.orders.front().type == OrderType::Repair) {
            const uint32_t tid = u.orders.front().target;
            const bool valid = tid < units_.size() && tid != u.id && units_[tid].alive && u.is_worker &&
                               units_[tid].repair_time.raw() > 0 && (units_[tid].attributes & kAttrMechanical) &&
                               units_[tid].hp < units_[tid].hp_max && are_allied(units_[tid].player, u.player);
            if (!valid) {
                u.orders.erase(u.orders.begin());
                u.repair_life = Fixed();
                u.repair_paid_m = u.repair_paid_g = 0;
                continue;
            }
            Unit& t = units_[tid];
            const Fixed reach = u.radius + t.radius + Fixed::from_ratio(1, 4);
            if (dist2(u, t) > reach * reach) {
                move_unit_toward(u, t.x, t.y, move_step(u, dt));
                continue;
            }
            Fixed heal = (t.hp_max / t.repair_time) * dt;
            if (heal > t.hp_max - t.hp) heal = t.hp_max - t.hp;
            const Fixed life = u.repair_life + heal;
            auto owed = [&](int32_t cost) {
                const int64_t num = int64_t(life.raw()) * cost, den = int64_t(t.hp_max.raw()) * 4;
                return den > 0 ? int32_t((num + den - 1) / den) : 0;
            };
            const int32_t need_m = owed(t.cost_minerals) - u.repair_paid_m, need_g = owed(t.cost_gas) - u.repair_paid_g;
            Player& pl = player(u.player);
            if (need_m > pl.minerals || need_g > pl.gas) continue;
            pl.minerals -= need_m;
            pl.gas -= need_g;
            u.repair_paid_m += need_m;
            u.repair_paid_g += need_g;
            u.repair_life = life;
            last_progress_tick_ = tick_;
            t.hp = t.hp + heal;
            if (t.hp >= t.hp_max) {
                t.hp = t.hp_max;
                u.orders.erase(u.orders.begin());
                u.repair_life = Fixed();
                u.repair_paid_m = u.repair_paid_g = 0;
            }
            continue;
        }

        if (u.reveal_timer.raw() > 0) {
            u.reveal_timer = u.reveal_timer - dt;
            if (u.reveal_timer.raw() <= 0) { u.reveal_timer = Fixed(); u.reveal_to = -1; }
        }
        if (u.help_cd.raw() > 0) {
            u.help_cd = u.help_cd - dt;
            if (u.help_cd.raw() < 0) u.help_cd = Fixed();
        }
        if (u.death_off_creep.raw() >= 0 && u.alive && creep_.has_sources() &&
            !creep_.creep_near(u.x, u.y, u.death_off_creep)) {
            u.hp = Fixed();
            u.alive = false;
        }
        if (u.lift_lock.raw() > 0) {
            u.lift_lock = u.lift_lock - dt;
            if (u.lift_lock.raw() < 0) u.lift_lock = Fixed();
        }
        if (u.lift_t < u.lift_rise_delay + u.lift_rise) u.lift_t = u.lift_t + dt;
        if (u.wander_range.raw() > 0 && u.alive && u.orders.empty() && !u.constructing) {
            const bool conjoined = u.spawn_parent != kNoTarget && u.spawn_parent < units_.size() &&
                                   units_[u.spawn_parent].alive;
            if (!conjoined) {
                if (u.wander_timer.raw() > 0) {
                    u.wander_timer = u.wander_timer - dt;
                } else {
                    if (!u.wander_home_set || !u.wander_leash) {
                        u.wander_ox = u.x;
                        u.wander_oy = u.y;
                        u.wander_home_set = true;
                    }
                    Fixed dx, dy;
                    for (int tries = 0; tries < 8; ++tries) {
                        dx = u.wander_range * Fixed::from_ratio(int(rng_.below(2001)) - 1000, 1000);
                        dy = u.wander_range * Fixed::from_ratio(int(rng_.below(2001)) - 1000, 1000);
                        if (dx * dx + dy * dy <= u.wander_range * u.wander_range) break;
                        if (tries == 7) { dx = dx * Fixed::from_ratio(7, 10); dy = dy * Fixed::from_ratio(7, 10); }
                    }
                    issue(u.id, Order{OrderType::Move, u.wander_ox + dx, u.wander_oy + dy}, false, false);
                    u.wander_timer = u.wander_delay;
                }
            }
        }
        if (u.offcreep_period.raw() > 0 && u.alive && !u.constructing && creep_.has_sources()) {
            if (creep_.is_creep(u.x, u.y)) {
                u.offcreep_timer = u.offcreep_period;
            } else if ((u.offcreep_timer = u.offcreep_timer - dt).raw() <= 0) {
                u.offcreep_timer = u.offcreep_timer + u.offcreep_period;
                u.hp = u.hp - u.offcreep_amount;
                if (u.hp.raw() <= 0) {
                    u.hp = Fixed();
                    u.alive = false;
                }
            }
        }
        if (u.build_cd.raw() > 0) {
            u.build_cd = u.build_cd - dt;
            if (u.build_cd.raw() < 0) u.build_cd = Fixed();
        }
        u.from_rest = u.cur_speed.raw() == 0;
        if (u.cooldown.raw() > 0) {
            u.cooldown = u.cooldown - dt;
            u.cd_carry = Fixed();
            if (u.cooldown.raw() < 0) {
                u.cd_carry = Fixed() - u.cooldown;
                u.cooldown = Fixed();
            }
        } else {
            u.cd_carry = Fixed();
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
                const Fixed near = arrival_near(u);
                done = (dx * dx + dy * dy) <= near * near;
                if (!done && separation_ && !u.flying && !u.burrowed) {
                    const Fixed my_d2 = dx * dx + dy * dy;
                    if (crowd_max_tick_ != tick_) {
                        crowd_max_radius_ = Fixed();
                        for (const Unit& r : units_)
                            if (r.alive && !r.flying && r.radius > crowd_max_radius_) crowd_max_radius_ = r.radius;
                        crowd_max_tick_ = tick_;
                    }
                    const Fixed pair = u.radius + crowd_max_radius_;
                    const Fixed reach = pair * Fixed::from_int(5) + Fixed::from_ratio(8, 64);
                    if (my_d2 <= reach * reach)
                    for (const Unit& r : units_) {
                        if (r.id == u.id || !r.alive || r.flying || r.burrowed || r.speed.raw() == 0) continue;
                        if (!r.orders.empty() || r.moved_this_tick || !are_allied(r.player, u.player)) continue;
                        if (!r.arrived || !(r.arrive_x == o.tx) || !(r.arrive_y == o.ty)) continue;
                        const Fixed rx = o.tx - r.x, ry = o.ty - r.y;
                        if (rx * rx + ry * ry >= my_d2) continue;
                        const Fixed blob = (u.radius + r.radius) * Fixed::from_int(4);
                        if (rx * rx + ry * ry > blob * blob) continue;
                        const Fixed cx = r.x - u.x, cy = r.y - u.y;
                        const Fixed touch = u.radius + r.radius + Fixed::from_ratio(4, 64);
                        if (cx * cx + cy * cy <= touch * touch) { done = true; break; }
                    }
                }
            }
            if (!done) break;
            if (o.type == OrderType::Move || o.type == OrderType::AttackMove) {
                u.arrived = true;
                u.arrive_x = o.tx;
                u.arrive_y = o.ty;
            }
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
                u.ever_ordered = true;
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
                    if (!u.guarding) {
                        bool keep = false;
                        if (u.guard_set && leash_reset_.raw() > 0) {
                            const Fixed kx = u.x - u.guard_x, ky = u.y - u.guard_y;
                            keep = kx * kx + ky * ky <= leash_reset_ * leash_reset_;
                        }
                        if (!keep) { u.guard_x = u.x; u.guard_y = u.y; }
                        u.guard_set = true;
                        u.guarding = true;
                    }
                    if (atk != kNoTarget) {
                        const Fixed lx = units_[atk].x - u.guard_x, ly = units_[atk].y - u.guard_y;
                        const Fixed lr = leash_radius_.raw() > 0 ? leash_radius_ : u.acquire_range;
                        if (lx * lx + ly * ly > lr * lr)
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

        if (atk != kNoTarget && atk < units_.size() && units_[atk].alive && !untargetable(units_[atk])) {
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
            Fixed w_arc = u.wpn_fire_arc;
            Fixed w_rd0 = u.wpn_random_delay_min, w_rd1 = u.wpn_random_delay_max;
            std::uint8_t w_slot = 0;
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
                w_arc = u.wpn2_fire_arc;
                w_rd0 = u.wpn2_random_delay_min;
                w_rd1 = u.wpn2_random_delay_max;
                w_slot = 1;
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
                const bool facing_ok = advance_turn(u, tgt.x, tgt.y, w_arc);
                if (facing_ok && u.cooldown.raw() == 0 && w_dmg.raw() > 0 && !power_disabled(u) &&
                    !attack_suppressed(u)) {
                    if (attack_windup_ && w_dp.raw() > 0) {
                        u.windup_timer = w_dp;
                        u.windup_target = tgt.id;
                        u.windup_dmg = w_dmg;
                        u.windup_slot = w_slot;
                        u.windup_ar = w_ar;
                        u.windup_hits = w_hits;
                        u.windup_battr = w_battr;
                        u.windup_bamt = w_bamt;
                        u.windup_ps = projectile_travel_ ? w_ps : Fixed();
                    } else if (projectile_travel_ && w_ps.raw() > 0) {
                        queue_projectile(u.id, u.x, u.y, tgt.id, w_ps, w_dmg, w_battr, w_bamt, w_ar, w_hits,
                                         w_slot);
                    } else {
                        apply_weapon_fire(u.id, tgt.id, w_dmg, w_battr, w_bamt, w_hits, w_ar, w_slot);
                    }
                    u.cooldown = reload_after_shot(u, w_per, w_rd0, w_rd1) - u.cd_carry;
                    if (u.cooldown.raw() < 0) u.cooldown = Fixed();
                    u.cd_carry = Fixed();
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
            const Fixed bx = dx_dest - u.x, by = dy_dest - u.y;
            const Fixed left = isqrt(bx * bx + by * by) - arrival_near(u);
            move_unit_toward(u, dx_dest, dy_dest, move_step(u, dt, left.raw() > 0 ? left : Fixed()));
        }
    }
    step_bunker_fire(dt);

    for (int round = 0; round < 8 && !behavior_fx_.empty(); ++round) {
        std::vector<BehaviorFx> fx;
        fx.swap(behavior_fx_);
        for (const BehaviorFx& f : fx) {
            if (f.holder >= units_.size() || !units_[f.holder].alive) continue;
            EffectContext c;
            c.source = f.caster;
            c.target = f.holder;
            c.origin = f.origin;
            c.sx = c.tx = units_[f.holder].x;
            c.sy = c.ty = units_[f.holder].y;
            execute_effect(f.ref, c);
        }
    }
    if (!pending_spawns_.empty()) {
        static const int DX[8] = {1, 1, 0, -1, -1, -1, 0, 1};
        static const int DY[8] = {0, 1, 1, 1, 0, -1, -1, -1};
        for (const PendingSpawn& ps : pending_spawns_) {
            if (ps.proto < 0 || size_t(ps.proto) >= spawn_protos_.size()) continue;
            for (int i = 0; i < ps.count; ++i) {
                Unit s = spawn_protos_[ps.proto];
                s.player = ps.owner;
                s.spawn_parent = ps.parent;
                s.larva_injected = ps.injected;
                const bool is_shade = ps.phase_link != kNoTarget;
                const bool exact = is_shade || ps.gather_node != kNoTarget;
                const std::vector<std::pair<Fixed, Fixed>>* table =
                    (!exact && ps.proto >= 0 && ps.proto < int32_t(spawn_offsets_.size()) &&
                     !spawn_offsets_[std::size_t(ps.proto)].empty())
                        ? &spawn_offsets_[std::size_t(ps.proto)]
                        : nullptr;
                if (table) {
                    int slot = i;
                    if (ps.parent != kNoTarget) {
                        std::vector<bool> used(table->size(), false);
                        for (const Unit& c : units_)
                            if (c.alive && c.spawn_parent == ps.parent && c.spawn_slot >= 0 &&
                                std::size_t(c.spawn_slot) < used.size())
                                used[std::size_t(c.spawn_slot)] = true;
                        slot = 0;
                        while (std::size_t(slot) < used.size() && used[std::size_t(slot)]) ++slot;
                    }
                    s.spawn_slot = int32_t(std::size_t(slot) % table->size());
                    const std::pair<Fixed, Fixed>& o = (*table)[std::size_t(s.spawn_slot)];
                    s.x = ps.x + o.first;
                    s.y = ps.y + o.second;
                } else if (!exact && ps.range.raw() > 0) {
                    Fixed dx, dy;
                    for (int tries = 0; tries < 8; ++tries) {
                        dx = ps.range * Fixed::from_ratio(int(rng_.below(2001)) - 1000, 1000);
                        dy = ps.range * Fixed::from_ratio(int(rng_.below(2001)) - 1000, 1000);
                        if (dx * dx + dy * dy <= ps.range * ps.range) break;
                        if (tries == 7) { dx = dx * Fixed::from_ratio(7, 10); dy = dy * Fixed::from_ratio(7, 10); }
                    }
                    s.x = ps.x + dx;
                    s.y = ps.y + dy;
                } else {
                    s.x = exact ? ps.x : ps.x + Fixed::from_int(DX[i & 7]);
                    s.y = exact ? ps.y : ps.y + Fixed::from_int(DY[i & 7]);
                }
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
    if (!pending_addon_bodies_.empty()) {
        std::vector<uint32_t> hosts;
        hosts.swap(pending_addon_bodies_);
        for (uint32_t b : hosts) spawn_addon_body(b);
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
    if (!researched_.empty()) {
        std::vector<int32_t> players;
        players.reserve(researched_.size());
        for (const auto& kv : researched_) players.push_back(kv.first);
        std::sort(players.begin(), players.end());
        for (int32_t p : players) {
            h = mix(h, 0xE5EAu);
            h = mix(h, uint32_t(p));
            for (const std::string& id : researched_.at(p)) h = mix(h, type_identity(id));
        }
    }
    for (const PendingResearch& p : research_pending_) {
        h = mix(h, 0xE5EBu);
        h = mix(h, uint32_t(p.player));
        h = mix(h, type_identity(p.id));
        h = mix(h, uint32_t(p.timer.raw()));
    }
    for (const Unit& u : units_) {
        h = mix(h, u.id);
        h = mix(h, uint32_t(u.player));
        h = mix(h, uint32_t(u.x.raw()));
        h = mix(h, uint32_t(u.y.raw()));
        h = mix(h, uint32_t(u.push_release));
        h = mix(h, uint32_t(u.has_anchor));
        h = mix(h, uint32_t(u.anchor_x.raw()));
        h = mix(h, uint32_t(u.anchor_y.raw()));
        h = mix(h, uint32_t(u.push_dir_x.raw()));
        h = mix(h, uint32_t(u.push_dir_y.raw()));
        h = mix(h, uint32_t(u.ever_ordered));
        for (const Order& o : u.orders) {
            h = mix(h, uint32_t(o.type));
            h = mix(h, uint32_t(o.tx.raw()));
            h = mix(h, uint32_t(o.ty.raw()));
            h = mix(h, o.target);
            h = mix(h, o.ability);
            h = mix(h, uint32_t(o.px.raw()));
            h = mix(h, uint32_t(o.py.raw()));
        }
        if (!u.catalog_id.empty()) h = mix(h, type_identity(u.catalog_id));
        if (u.type_id != 0) h = mix(h, uint32_t(u.type_id));
        if (u.addon != kAddonNone) h = mix(h, uint32_t(u.addon) ^ 0xADD0u);
        if (u.addon_body != kNoTarget) h = mix(h, u.addon_body ^ 0xADDB0D1Eu);
        if (u.addon_body_kind != 0) h = mix(h, u.addon_host ^ (uint32_t(u.addon_body_kind) << 28));
        if (u.has_rally) {
            h = mix(h, uint32_t(u.rally_x.raw()));
            h = mix(h, uint32_t(u.rally_y.raw()));
        }
        if (u.air_expand) { h = mix(h, 0xA13Eu); h = mix(h, uint32_t(u.air_expand)); }
        h = mix(h, uint32_t(u.hp.raw()));
        h = mix(h, uint32_t(u.shields.raw()));
        h = mix(h, uint32_t(u.cooldown.raw()));
        h = mix(h, u.target);
        h = mix(h, u.alive ? 1u : 0u);
        h = mix(h, u.flying ? 2u : 0u);
        if (u.lowered) h = mix(h, 0x10ED0u);
        if (u.land_pending) {
            h = mix(h, 0x1A4Du);
            h = mix(h, uint32_t(u.land_x.raw()));
            h = mix(h, uint32_t(u.land_y.raw()));
            h = mix(h, uint32_t(u.land_timer.raw()));
        }
        h = mix(h, uint32_t(u.resource_left));
        h = mix(h, uint32_t(u.carrying));
        h = mix(h, uint32_t(u.mine_cd.raw()));
        if (u.return_cd.raw() != 0) h = mix(h, uint32_t(u.return_cd.raw()));
        h = mix(h, u.gather_target);
        if (u.is_gas) h = mix(h, 0x6A5Eu);
        if (u.gathers_gas) h = mix(h, 0x6A5Fu);
        if (u.supply_uses.raw() != 0) h = mix(h, uint32_t(u.supply_uses.raw()) ^ 0x50F1u);
        if (u.supply_provides.raw() != 0) h = mix(h, uint32_t(u.supply_provides.raw()) ^ 0x50F2u);
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
            if (u.guard_set) h = mix(h, 2u);
        }
        if (u.reveal_timer.raw() > 0) {
            h = mix(h, uint32_t(u.reveal_timer.raw()));
            h = mix(h, uint32_t(u.reveal_to));
        }
        if (u.help_cd.raw() > 0) h = mix(h, uint32_t(u.help_cd.raw()));
        if (u.repair_life.raw() > 0) {
            h = mix(h, uint32_t(u.repair_life.raw()));
            h = mix(h, uint32_t(u.repair_paid_m));
            h = mix(h, uint32_t(u.repair_paid_g));
        }
        if (u.windup_timer.raw() > 0) {
            h = mix(h, uint32_t(u.windup_timer.raw()));
            h = mix(h, u.windup_target);
            h = mix(h, uint32_t(u.windup_dmg.raw()));
            h = mix(h, u.windup_battr);
            h = mix(h, uint32_t(u.windup_bamt.raw()));
            h = mix(h, uint32_t(u.windup_ps.raw()));
            h = mix(h, uint32_t(u.windup_ar.raw()));
            h = mix(h, uint32_t(u.windup_hits));
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
        if (u.armor.raw() != 0) { h = mix(h, 0xA401u); h = mix(h, uint32_t(u.armor.raw())); }
        if (u.shield_armor.raw() != 0) { h = mix(h, 0xA402u); h = mix(h, uint32_t(u.shield_armor.raw())); }
        if (u.speed.raw() != 0) { h = mix(h, 0xA403u); h = mix(h, uint32_t(u.speed.raw())); }
        if (u.wpn_damage.raw() != 0) { h = mix(h, 0xA404u); h = mix(h, uint32_t(u.wpn_damage.raw())); }
        if (u.wpn_range.raw() != 0) { h = mix(h, 0xA405u); h = mix(h, uint32_t(u.wpn_range.raw())); }
        if (u.wpn_period.raw() != 0) { h = mix(h, 0xA406u); h = mix(h, uint32_t(u.wpn_period.raw())); }
        if (u.wpn2_damage.raw() != 0) { h = mix(h, 0xA407u); h = mix(h, uint32_t(u.wpn2_damage.raw())); }
        if (u.wpn2_range.raw() != 0) { h = mix(h, 0xA408u); h = mix(h, uint32_t(u.wpn2_range.raw())); }
        if (u.wpn2_period.raw() != 0) { h = mix(h, 0xA409u); h = mix(h, uint32_t(u.wpn2_period.raw())); }
        if (u.energy_max.raw() != 0) { h = mix(h, 0xA40Au); h = mix(h, uint32_t(u.energy_max.raw())); }
        if (u.wpn_bonus_attr != 0) { h = mix(h, 0xA40Bu); h = mix(h, uint32_t(u.wpn_bonus_attr)); }
        if (u.wpn_bonus_amount.raw() != 0) { h = mix(h, 0xA40Cu); h = mix(h, uint32_t(u.wpn_bonus_amount.raw())); }
        if (u.dmg_fraction.raw() != Fixed::from_int(1).raw()) h = mix(h, uint32_t(u.dmg_fraction.raw()));
        if (u.kills != 0) h = mix(h, uint32_t(u.kills) ^ 0x4B11u);
        if (u.deals_no_damage) h = mix(h, 0x11A11Fu);
        if (u.dmg_clamp_max.raw() != 0) h = mix(h, uint32_t(u.dmg_clamp_max.raw()));
        if (u.dmg_minimum.raw() != 0) h = mix(h, uint32_t(u.dmg_minimum.raw()));
        if (u.aura_reduction.raw() != 0) h = mix(h, uint32_t(u.aura_reduction.raw()));
        if (u.needs_power) h = mix(h, u.powered ? 0x50E1u : 0x50E0u);
        if (u.burns_down && u.burn_timer.raw() > 0) h = mix(h, uint32_t(u.burn_timer.raw()));
        if (u.loaded) { h = mix(h, 0x10ADu); h = mix(h, u.transport_id); }
        if (u.burrowed) h = mix(h, 0xB020u);
        if (u.burrowed_wpn_on) h = mix(h, 0xB021u);
        if (u.field_cloaked) h = mix(h, 0xC10Bu);
        if (u.time_life.raw() > 0) h = mix(h, uint32_t(u.time_life.raw()));
        if (u.phase_timer.raw() > 0) h = mix(h, uint32_t(u.phase_timer.raw()) ^ 0xADE7u);
        if (u.build_charges >= 0)
            h = mix(h, (uint32_t(u.build_charges) << 24) ^ uint32_t(u.build_cd.raw()) ^ 0xC4A6u);
        if (u.salvage_timer.raw() > 0) {
            h = mix(h, uint32_t(u.salvage_timer.raw()) ^ 0x5A17u);
            h = mix(h, uint32_t(u.salvage_minerals));
            h = mix(h, uint32_t(u.salvage_gas));
        }
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
            if (b.permanent) h = mix(h, b.src);
            h = mix(h, uint32_t(b.speed_mult.raw()));
            h = mix(h, uint32_t((b.root ? 1u : 0u) | (b.stun ? 2u : 0u) | (b.lift ? 4u : 0u)));
        }
    }
    for (const Player& p : players_) {
        h = mix(h, uint32_t(p.minerals));
        h = mix(h, uint32_t(p.gas));
        if (p.team != -1) h = mix(h, uint32_t(p.team));
    }
    for (const Pending& pd : pending_) {
        h = mix(h, pd.producer);
        h = mix(h, uint32_t(pd.timer.raw()));
        if (!pd.proto.catalog_id.empty()) h = mix(h, type_identity(pd.proto.catalog_id));
        if (pd.proto.type_id != 0) h = mix(h, uint32_t(pd.proto.type_id));
        if (pd.count != 1) h = mix(h, uint32_t(pd.count));
        if (pd.has_pos) {
            h = mix(h, uint32_t(pd.px.raw()));
            h = mix(h, uint32_t(pd.py.raw()));
        }
    }
    for (const WorkerBuild& wb : worker_builds_) {
        h = mix(h, wb.builder);
        h = mix(h, uint32_t(wb.x.raw()));
        h = mix(h, uint32_t(wb.y.raw()));
        if (!wb.proto.catalog_id.empty()) h = mix(h, type_identity(wb.proto.catalog_id));
        if (wb.proto.type_id != 0) h = mix(h, uint32_t(wb.proto.type_id));
        h = mix(h, uint32_t(wb.build_time.raw()));
        h = mix(h, uint32_t(wb.arrive.raw()));
        if (wb.verb != BuildVerb::Committed) h = mix(h, uint32_t(wb.verb) ^ 0xB1DEu);
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
        if (hb.vital != HealVital::Life) h = mix(h, uint32_t(hb.vital));
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
        h = mix(h, uint32_t(p.hits));
    }
    h = mix(h, creep_.checksum());
    return h;
}

std::vector<World::ShotView> World::shots_in_flight() const {
    std::vector<ShotView> out;
    out.reserve(pending_hits_.size());
    for (const PendingHit& p : pending_hits_) {
        ShotView v;
        v.attacker = p.attacker;
        v.target = p.target;
        v.x = p.px;
        v.y = p.py;
        v.slot = p.slot;
        out.push_back(v);
    }
    return out;
}

}
