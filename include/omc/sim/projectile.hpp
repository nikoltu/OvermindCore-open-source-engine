#pragma once

#include <algorithm>
#include <cstdint>
#include <utility>
#include <vector>

#include "omc/sim/weapon.hpp"
#include "omc/sim/world.hpp"

namespace omc::sim {

struct Projectile {
    Fixed x, y;
    std::uint32_t target = kNoTarget;
    std::uint32_t attacker = kNoTarget;
    std::int32_t owner = -1;
    WeaponDef weapon;
    bool alive = true;
};

class ProjectilePool {
public:
    void spawn(Fixed fx, Fixed fy, std::uint32_t attacker, std::int32_t owner,
               std::uint32_t target, const WeaponDef& weapon) {
        Projectile p;
        p.x = fx;
        p.y = fy;
        p.attacker = attacker;
        p.owner = owner;
        p.target = target;
        p.weapon = weapon;
        shots_.push_back(p);
    }

    std::size_t live() const {
        std::size_t n = 0;
        for (const Projectile& p : shots_)
            if (p.alive) ++n;
        return n;
    }

    void advance(World& world, Fixed dt) {
        for (Projectile& p : shots_) {
            if (!p.alive) continue;

            Fixed tx = p.x, ty = p.y;
            const bool target_live = p.target < world.units().size() && world.unit(p.target).alive;
            if (target_live) {
                tx = world.unit(p.target).x;
                ty = world.unit(p.target).y;
            }

            const Fixed speed = p.weapon.projectile_speed;
            bool reached = false;
            if (speed.raw() <= 0) {
                reached = true;
                p.x = tx;
                p.y = ty;
            } else {
                const Fixed step = speed * dt;
                const Fixed dx = tx - p.x, dy = ty - p.y;
                const Fixed d2 = dx * dx + dy * dy;
                if (d2 <= step * step) {
                    p.x = tx;
                    p.y = ty;
                    reached = true;
                } else {
                    const Fixed d = isqrt(d2);
                    if (d.raw() > 0) {
                        p.x = p.x + dx * step / d;
                        p.y = p.y + dy * step / d;
                    }
                }
            }
            if (reached) {
                impact(world, p);
                p.alive = false;
            }
        }
        std::size_t w = 0;
        for (std::size_t r = 0; r < shots_.size(); ++r)
            if (shots_[r].alive) shots_[w++] = shots_[r];
        shots_.resize(w);
    }

    int intercept(Fixed x, Fixed y, Fixed radius, std::int32_t defender_player, int max) {
        if (max <= 0 || radius.raw() <= 0) return 0;
        const Fixed r2 = radius * radius;
        std::vector<std::pair<Fixed, std::size_t>> cand;
        for (std::size_t i = 0; i < shots_.size(); ++i) {
            Projectile& p = shots_[i];
            if (!p.alive || p.owner == defender_player) continue;
            const Fixed dx = p.x - x, dy = p.y - y;
            const Fixed d2 = dx * dx + dy * dy;
            if (d2.raw() > r2.raw()) continue;
            cand.emplace_back(d2, i);
        }
        std::stable_sort(cand.begin(), cand.end(),
                         [](const auto& a, const auto& b) { return a.first.raw() < b.first.raw(); });
        int killed = 0;
        for (const auto& c : cand) {
            if (killed >= max) break;
            shots_[c.second].alive = false;
            ++killed;
        }
        return killed;
    }

private:
    void impact(World& world, const Projectile& p) {
        const WeaponDef& wd = p.weapon;
        if (wd.splash == SplashShape::None) {
            if (p.target >= world.units().size()) return;
            Unit& t = world.unit(p.target);
            if (!t.alive) return;
            if (!weapon_can_target(wd, t.flying)) return;
            apply(world, p.attacker, t, weapon_effective_damage(wd, t.attributes, t.armor));
            if (wd.bounce_count > 0) bounce(world, p, p.target);
            return;
        }
        const std::uint32_t n = std::uint32_t(world.units().size());
        for (std::uint32_t i = 0; i < n; ++i) {
            Unit& t = world.unit(i);
            if (!t.alive) continue;
            if (world.are_allied(t.player, p.owner)) continue;
            if (!weapon_can_target(wd, t.flying)) continue;
            const Fixed dx = t.x - p.x, dy = t.y - p.y;
            const Fixed dist = isqrt(dx * dx + dy * dy);
            const Fixed factor = splash_factor(wd, dist);
            if (factor.raw() <= 0) continue;
            Fixed dmg = weapon_effective_damage(wd, t.attributes, t.armor) * factor;
            if (dmg.raw() <= 0) continue;
            apply(world, p.attacker, t, dmg);
        }
    }

    void bounce(World& world, const Projectile& p, std::uint32_t first) {
        const WeaponDef& wd = p.weapon;
        const std::uint32_t n = std::uint32_t(world.units().size());
        std::vector<std::uint8_t> hit(n, 0);
        if (first < n) hit[first] = 1;
        std::uint32_t from = first;
        Fixed dmg = weapon_effective_damage(wd, first < n ? world.unit(first).attributes : 0,
                                            first < n ? world.unit(first).armor : Fixed());
        const int64_t rr = wd.bounce_range.raw();
        const int64_t r2 = rr * rr;
        for (int b = 0; b < wd.bounce_count; ++b) {
            const Unit& src = world.unit(from);
            const Fixed fx = src.x, fy = src.y;
            std::uint32_t best = kNoTarget;
            int64_t best_d2 = 0;
            for (std::uint32_t i = 0; i < n; ++i) {
                if (hit[i]) continue;
                const Unit& t = world.unit(i);
                if (!t.alive) continue;
                if (world.are_allied(t.player, p.owner)) continue;
                if (!weapon_can_target(wd, t.flying)) continue;
                const int64_t ddx = int64_t((t.x - fx).raw()), ddy = int64_t((t.y - fy).raw());
                const int64_t d2 = ddx * ddx + ddy * ddy;
                if (rr > 0 && d2 > r2) continue;
                if (best == kNoTarget || d2 < best_d2) {
                    best = i;
                    best_d2 = d2;
                }
            }
            if (best == kNoTarget) break;
            dmg = dmg * wd.bounce_falloff;
            if (dmg.raw() <= 0) break;
            apply(world, p.attacker, world.unit(best), dmg);
            hit[best] = 1;
            from = best;
        }
    }

    static void apply(World& world, std::uint32_t attacker, Unit& t, Fixed dmg) {
        (void)attacker;
        if (t.shields.raw() > 0) {
            if (t.shields >= dmg) {
                t.shields = t.shields - dmg;
                return;
            }
            dmg = dmg - t.shields;
            t.shields = Fixed();
        }
        t.hp = t.hp - dmg;
        if (t.hp.raw() <= 0) t.alive = false;
        (void)world;
    }

    std::vector<Projectile> shots_;
};

}
