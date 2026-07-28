#include "sc2/sim/world.hpp"

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

uint32_t World::acquire_target(const Unit& u) const {
    uint32_t best = kNoTarget;
    Fixed best_d2;
    for (const Unit& o : units_) {
        if (!o.alive || o.player == u.player) continue;
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
    return speed * dt;
}

bool World::move_unit_toward(Unit& u, Fixed gx, Fixed gy, Fixed step) {
    if (!nav_.has_blocked()) {
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
            const Fixed push = pen / Fixed::from_int(4);
            const Fixed px = nx * push, py = ny * push;
            ax[i] = ax[i] - px;
            ay[i] = ay[i] - py;
            ax[j] = ax[j] + px;
            ay[j] = ay[j] + py;
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

Fixed World::supply_used(int32_t p) const {
    Fixed s;
    for (const Unit& u : units_)
        if (u.alive && u.player == p) s = s + u.supply_uses;
    return s;
}

Fixed World::supply_max(int32_t p) const {
    Fixed s;
    for (const Unit& u : units_)
        if (u.alive && u.player == p) s = s + u.supply_provides;
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

void World::step_cast(Unit& u, Fixed dt) {
    Order& o = u.orders.front();
    if (o.ability >= abilities_.size()) {
        u.orders.erase(u.orders.begin());
        return;
    }
    const AbilityDef ab = abilities_[o.ability];
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
    if (producer >= units_.size()) return false;
    int32_t pl = units_[producer].player;
    Player& pp = player(pl);
    pp.minerals -= mineral_cost;
    pp.gas -= gas_cost;
    Unit u = proto;
    u.player = pl;
    pending_.push_back(Pending{producer, u, build_time});
    return true;
}

void World::step_production(Fixed dt) {
    for (size_t i = 0; i < pending_.size();) {
        Pending& pd = pending_[i];
        pd.timer = pd.timer - dt;
        if (pd.timer.raw() <= 0) {
            Unit u = pd.proto;
            if (pd.producer < units_.size()) {
                const Unit& pr = units_[pd.producer];
                u.x = pr.x + pr.radius + Fixed::from_int(1);
                u.y = pr.y;
            }
            add_unit(u);
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
                int32_t amt = u.carry_amount > 0 ? u.carry_amount : 5;
                if (m.resource_left < amt) amt = m.resource_left;
                u.carrying = amt;
                m.resource_left -= amt;
            }
        }
        return;
    }

    if (u.gather_target == kNoTarget || u.gather_target >= units_.size() ||
        units_[u.gather_target].resource_left <= 0) {
        uint32_t best = kNoTarget;
        Fixed bd;
        for (const Unit& o : units_) {
            if (o.resource_left <= 0) continue;
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
        Fixed reach = u.radius + d.radius + Fixed::from_int(1);
        if (dist2(u, d) <= reach * reach) {
            player(u.player).minerals += u.carrying;
            u.carrying = 0;
        } else {
            move_unit_toward(u, d.x, d.y, move_step(u, dt));
        }
    } else if (u.gather_target < units_.size()) {
        Unit& m = units_[u.gather_target];
        Fixed reach = u.radius + m.radius + Fixed::from_int(1);
        if (dist2(u, m) <= reach * reach) {
            u.mine_cd = u.mine_time.raw() > 0 ? u.mine_time : Fixed::parse("2.786");
        } else {
            move_unit_toward(u, m.x, m.y, move_step(u, dt));
        }
    }
}

void World::step() {
    const Fixed dt = World::dt();
    creep_.tick();
    step_production(dt);
    for (Unit& u : units_) {
        if (!u.alive) continue;

        if (u.buff_timer.raw() > 0) {
            u.buff_timer = u.buff_timer - dt;
            if (u.buff_timer.raw() <= 0) {
                u.buff_timer = Fixed();
                u.wpn_damage = u.wpn_damage - u.buff_damage;
                u.buff_damage = Fixed();
            }
        }

        if (!u.orders.empty() && u.orders.front().type == OrderType::Gather) {
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
            case OrderType::Idle:
            default:
                atk = acquire_target(u);
                break;
        }

        u.target = atk;

        if (atk != kNoTarget && atk < units_.size() && units_[atk].alive) {
            Unit& tgt = units_[atk];
            Fixed reach = u.wpn_range + u.radius + tgt.radius;
            bool in_range = dist2(u, tgt) <= reach * reach;
            if (in_range) {
                if (u.cooldown.raw() == 0 && u.wpn_damage.raw() > 0) {
                    Defender d{tgt.hp, tgt.shields, tgt.armor};
                    apply_hit(Weapon{u.wpn_damage, u.wpn_period}, d);
                    tgt.hp = d.life;
                    tgt.shields = d.shields;
                    if (tgt.hp.raw() <= 0 && tgt.alive) {
                        tgt.alive = false;
                        award_kill_xp(u, tgt.kill_xp);
                    }
                    u.cooldown = u.wpn_period;
                }
            } else if (can_move) {
                move_unit_toward(u, tgt.x, tgt.y, move_step(u, dt));
            }
        } else if (has_dest && can_move) {
            move_unit_toward(u, dx_dest, dy_dest, move_step(u, dt));
        }
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
        h = mix(h, u.gather_target);
        h = mix(h, uint64_t(u.xp));
        h = mix(h, uint32_t(u.level));
        h = mix(h, uint32_t(u.hp_max.raw()));
        if (u.buff_timer.raw() != 0) {
            h = mix(h, uint32_t(u.buff_timer.raw()));
            h = mix(h, uint32_t(u.buff_damage.raw()));
        }
    }
    for (const Player& p : players_) {
        h = mix(h, uint32_t(p.minerals));
        h = mix(h, uint32_t(p.gas));
    }
    for (const Pending& pd : pending_) {
        h = mix(h, pd.producer);
        h = mix(h, uint32_t(pd.timer.raw()));
    }
    h = mix(h, creep_.checksum());
    return h;
}

}
