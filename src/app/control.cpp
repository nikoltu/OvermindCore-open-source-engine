#include "sc2/app/control.hpp"

#include <algorithm>
#include <cmath>

#include "sc2/sim/weapon.hpp"

namespace sc2::app {

using sim::Fixed;
using sim::kNoTarget;
using sim::Order;
using sim::OrderType;

namespace {

constexpr float kSelectHeightPerRadius = 3.0f;

bool invert4(const std::array<float, 16>& m, std::array<float, 16>& out) {
    std::array<float, 16> inv;
    inv[0] = m[5] * m[10] * m[15] - m[5] * m[11] * m[14] - m[9] * m[6] * m[15] +
             m[9] * m[7] * m[14] + m[13] * m[6] * m[11] - m[13] * m[7] * m[10];
    inv[4] = -m[4] * m[10] * m[15] + m[4] * m[11] * m[14] + m[8] * m[6] * m[15] -
             m[8] * m[7] * m[14] - m[12] * m[6] * m[11] + m[12] * m[7] * m[10];
    inv[8] = m[4] * m[9] * m[15] - m[4] * m[11] * m[13] - m[8] * m[5] * m[15] +
             m[8] * m[7] * m[13] + m[12] * m[5] * m[11] - m[12] * m[7] * m[9];
    inv[12] = -m[4] * m[9] * m[14] + m[4] * m[10] * m[13] + m[8] * m[5] * m[14] -
              m[8] * m[6] * m[13] - m[12] * m[5] * m[10] + m[12] * m[6] * m[9];
    inv[1] = -m[1] * m[10] * m[15] + m[1] * m[11] * m[14] + m[9] * m[2] * m[15] -
             m[9] * m[3] * m[14] - m[13] * m[2] * m[11] + m[13] * m[3] * m[10];
    inv[5] = m[0] * m[10] * m[15] - m[0] * m[11] * m[14] - m[8] * m[2] * m[15] +
             m[8] * m[3] * m[14] + m[12] * m[2] * m[11] - m[12] * m[3] * m[10];
    inv[9] = -m[0] * m[9] * m[15] + m[0] * m[11] * m[13] + m[8] * m[1] * m[15] -
             m[8] * m[3] * m[13] - m[12] * m[1] * m[11] + m[12] * m[3] * m[9];
    inv[13] = m[0] * m[9] * m[14] - m[0] * m[10] * m[13] - m[8] * m[1] * m[14] +
              m[8] * m[2] * m[13] + m[12] * m[1] * m[10] - m[12] * m[2] * m[9];
    inv[2] = m[1] * m[6] * m[15] - m[1] * m[7] * m[14] - m[5] * m[2] * m[15] +
             m[5] * m[3] * m[14] + m[13] * m[2] * m[7] - m[13] * m[3] * m[6];
    inv[6] = -m[0] * m[6] * m[15] + m[0] * m[7] * m[14] + m[4] * m[2] * m[15] -
             m[4] * m[3] * m[14] - m[12] * m[2] * m[7] + m[12] * m[3] * m[6];
    inv[10] = m[0] * m[5] * m[15] - m[0] * m[7] * m[13] - m[4] * m[1] * m[15] +
              m[4] * m[3] * m[13] + m[12] * m[1] * m[7] - m[12] * m[3] * m[5];
    inv[14] = -m[0] * m[5] * m[14] + m[0] * m[6] * m[13] + m[4] * m[1] * m[14] -
              m[4] * m[2] * m[13] - m[12] * m[1] * m[6] + m[12] * m[2] * m[5];
    inv[3] = -m[1] * m[6] * m[11] + m[1] * m[7] * m[10] + m[5] * m[2] * m[11] -
             m[5] * m[3] * m[10] - m[9] * m[2] * m[7] + m[9] * m[3] * m[6];
    inv[7] = m[0] * m[6] * m[11] - m[0] * m[7] * m[10] - m[4] * m[2] * m[11] +
             m[4] * m[3] * m[10] + m[8] * m[2] * m[7] - m[8] * m[3] * m[6];
    inv[11] = -m[0] * m[5] * m[11] + m[0] * m[7] * m[9] + m[4] * m[1] * m[11] -
              m[4] * m[3] * m[9] - m[8] * m[1] * m[7] + m[8] * m[3] * m[5];
    inv[15] = m[0] * m[5] * m[10] - m[0] * m[6] * m[9] - m[4] * m[1] * m[10] +
              m[4] * m[2] * m[9] + m[8] * m[1] * m[6] - m[8] * m[2] * m[5];

    float det = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];
    if (std::fabs(det) < 1e-20f) return false;
    float invdet = 1.0f / det;
    for (int i = 0; i < 16; ++i) out[i] = inv[i] * invdet;
    return true;
}

void transform_point(const std::array<float, 16>& m, float x, float y, float z,
                     float out[4]) {
    out[0] = m[0] * x + m[4] * y + m[8] * z + m[12];
    out[1] = m[1] * x + m[5] * y + m[9] * z + m[13];
    out[2] = m[2] * x + m[6] * y + m[10] * z + m[14];
    out[3] = m[3] * x + m[7] * y + m[11] * z + m[15];
}

Fixed float_to_fixed(float v) {
    return Fixed::from_raw(int32_t(std::lround(double(v) * Fixed::kOne)));
}

}

Control::Control(const render::Mat4& mvp, uint32_t view_w, uint32_t view_h,
                 int32_t player)
    : mvp_(mvp), view_w_(view_w), view_h_(view_h), player_(player) {
    invertible_ = invert4(mvp_.m, inv_mvp_.m);
}

bool Control::project(float x, float y, float z, float& px, float& py) const {
    const auto& m = mvp_.m;
    float cx = m[0] * x + m[4] * y + m[8] * z + m[12];
    float cy = m[1] * x + m[5] * y + m[9] * z + m[13];
    float cw = m[3] * x + m[7] * y + m[11] * z + m[15];
    if (cw <= 1e-6f) return false;
    px = (cx / cw * 0.5f + 0.5f) * float(view_w_);
    py = (cy / cw * 0.5f + 0.5f) * float(view_h_);
    return true;
}

std::vector<uint32_t> Control::box_select(const sim::World& world, const ScreenRect& rect,
                                          bool marquee) const {
    float minx = std::min(rect.x0, rect.x1), maxx = std::max(rect.x0, rect.x1);
    float miny = std::min(rect.y0, rect.y1), maxy = std::max(rect.y0, rect.y1);

    std::vector<uint32_t> hits;
    for (const sim::Unit& u : world.units()) {
        if (!u.alive || u.player != player_) continue;
        const float ux = float(u.x.to_double()), uy = float(u.y.to_double());
        const float fz = ground_z(ux, uy);
        float fx, fy;
        if (!project(ux, uy, fz, fx, fy)) continue;
        float xlo = fx, xhi = fx, ylo = fy, yhi = fy;
        float tx, ty;
        if (project(ux, uy, fz + float(u.radius.to_double()) * kSelectHeightPerRadius, tx, ty)) {
            xlo = std::min(fx, tx); xhi = std::max(fx, tx);
            ylo = std::min(fy, ty); yhi = std::max(fy, ty);
        }
        if (xhi >= minx && xlo <= maxx && yhi >= miny && ylo <= maxy) hits.push_back(u.id);
    }
    if (marquee) {
        bool any_mobile = false;
        for (std::uint32_t id : hits)
            if (id < world.units().size() &&
                (world.units()[id].attributes & sim::kAttrStructure) == 0) {
                any_mobile = true;
                break;
            }
        if (any_mobile) {
            std::vector<std::uint32_t> mobile;
            mobile.reserve(hits.size());
            for (std::uint32_t id : hits)
                if (id < world.units().size() &&
                    (world.units()[id].attributes & sim::kAttrStructure) == 0)
                    mobile.push_back(id);
            hits.swap(mobile);
        }
    }
    return hits;
}

bool Control::ground_point(const sim::World&, const ScreenPoint& p, sim::Fixed& wx,
                           sim::Fixed& wy) const {
    if (!invertible_ || view_w_ == 0 || view_h_ == 0) return false;

    float ndc_x = (p.x / float(view_w_)) * 2.0f - 1.0f;
    float ndc_y = (p.y / float(view_h_)) * 2.0f - 1.0f;

    float n[4], f[4];
    transform_point(inv_mvp_.m, ndc_x, ndc_y, 0.0f, n);
    transform_point(inv_mvp_.m, ndc_x, ndc_y, 1.0f, f);
    if (std::fabs(n[3]) < 1e-12f || std::fabs(f[3]) < 1e-12f) return false;
    float nx = n[0] / n[3], ny = n[1] / n[3], nz = n[2] / n[3];
    float fx = f[0] / f[3], fy = f[1] / f[3], fz = f[2] / f[3];

    float dz = fz - nz;
    if (std::fabs(dz) < 1e-9f) return false;
    float t = -nz / dz;
    if (t < 0.0f) return false;

    if (ground_) {
        const float top = ground_->max_z();
        if (top > 0.0f) {
            auto ray = [&](float u, float& x, float& y, float& z) {
                x = nx + u * (fx - nx);
                y = ny + u * (fy - ny);
                z = nz + u * (fz - nz);
            };
            const float t_top = std::min((top - nz) / dz, t);
            float t0 = std::max(0.0f, t_top);
            if (t0 <= t) {
                float ax, ay, az, bx, by, bz;
                ray(t0, ax, ay, az);
                ray(t, bx, by, bz);
                const float span = std::sqrt((bx - ax) * (bx - ax) + (by - ay) * (by - ay));
                int steps = int(span) + 1;
                if (steps < 1) steps = 1;
                if (steps > 4096) steps = 4096;

                float prev_u = t0, prev_gap = az - ground_->z_at(ax, ay);
                bool hit = false;
                float hit_lo = t0, hit_hi = t0;
                if (prev_gap <= 0.0f) {
                    hit = true;
                } else {
                    for (int i = 1; i <= steps; ++i) {
                        const float u = t0 + (t - t0) * (float(i) / float(steps));
                        float sx, sy, sz;
                        ray(u, sx, sy, sz);
                        const float gap = sz - ground_->z_at(sx, sy);
                        if (gap <= 0.0f) {
                            hit = true;
                            hit_lo = prev_u;
                            hit_hi = u;
                            break;
                        }
                        prev_u = u;
                        prev_gap = gap;
                    }
                }
                if (hit) {
                    for (int i = 0; i < 16; ++i) {
                        const float mid = 0.5f * (hit_lo + hit_hi);
                        float sx, sy, sz;
                        ray(mid, sx, sy, sz);
                        if (sz - ground_->z_at(sx, sy) > 0.0f) hit_lo = mid;
                        else hit_hi = mid;
                    }
                    t = hit_hi;
                }
            }
        }
    }

    wx = float_to_fixed(nx + t * (fx - nx));
    wy = float_to_fixed(ny + t * (fy - ny));
    return true;
}

uint32_t Control::pick_target(const sim::World& world, const ScreenPoint& p) const {
    Fixed gx, gy;
    if (!ground_point(world, p, gx, gy)) return kNoTarget;

    uint32_t best = kNoTarget;
    Fixed best_d2;
    for (const sim::Unit& u : world.units()) {
        if (!u.alive || u.player == player_ || u.resource_left > 0) continue;
        Fixed r = std::max(u.radius, pick_radius_);
        Fixed dx = u.x - gx, dy = u.y - gy;
        Fixed d2 = dx * dx + dy * dy;
        if (d2 > r * r) continue;
        if (best == kNoTarget || d2 < best_d2) {
            best = u.id;
            best_d2 = d2;
        }
    }
    return best;
}

uint32_t Control::pick_ally(const sim::World& world, const ScreenPoint& p) const {
    Fixed gx, gy;
    if (!ground_point(world, p, gx, gy)) return kNoTarget;
    uint32_t best = kNoTarget;
    Fixed best_d2;
    for (const sim::Unit& u : world.units()) {
        if (!u.alive || u.resource_left > 0 || !world.are_allied(u.player, player_)) continue;
        const Fixed r = std::max(u.radius, pick_radius_);
        const Fixed dx = u.x - gx, dy = u.y - gy, d2 = dx * dx + dy * dy;
        if (d2 > r * r) continue;
        if (best == kNoTarget || d2 < best_d2) { best = u.id; best_d2 = d2; }
    }
    return best;
}

uint32_t Control::pick_own(const sim::World& world, const ScreenPoint& p) const {
    Fixed gx, gy;
    if (!ground_point(world, p, gx, gy)) return kNoTarget;

    uint32_t best = kNoTarget;
    Fixed best_d2;
    for (const sim::Unit& u : world.units()) {
        if (!u.alive || u.player != player_ || u.resource_left > 0) continue;
        Fixed r = std::max(u.radius, pick_radius_);
        Fixed dx = u.x - gx, dy = u.y - gy;
        Fixed d2 = dx * dx + dy * dy;
        if (d2 > r * r) continue;
        if (best == kNoTarget || d2 < best_d2) {
            best = u.id;
            best_d2 = d2;
        }
    }
    return best;
}

uint32_t Control::pick_resource(const sim::World& world, const ScreenPoint& p) const {
    Fixed gx, gy;
    if (!ground_point(world, p, gx, gy)) return kNoTarget;

    uint32_t best = kNoTarget;
    Fixed best_d2;
    for (const sim::Unit& u : world.units()) {
        if (!u.alive || u.resource_left <= 0) continue;
        Fixed r = std::max(u.radius, pick_radius_);
        Fixed dx = u.x - gx, dy = u.y - gy;
        Fixed d2 = dx * dx + dy * dy;
        if (d2 > r * r) continue;
        if (best == kNoTarget || d2 < best_d2) {
            best = u.id;
            best_d2 = d2;
        }
    }
    return best;
}

Command Control::resolve_command(const sim::World& world, const ScreenPoint& p,
                                 bool attack_move) const {
    Command cmd;
    uint32_t enemy = pick_target(world, p);
    if (enemy != kNoTarget) {
        cmd.kind = CommandKind::Attack;
        cmd.target = enemy;
        return cmd;
    }
    uint32_t node = pick_resource(world, p);
    if (node != kNoTarget) {
        cmd.kind = CommandKind::Gather;
        cmd.target = node;
        return cmd;
    }
    Fixed gx, gy;
    if (ground_point(world, p, gx, gy)) {
        if (!attack_move) {
            const uint32_t ally = pick_ally(world, p);
            if (ally != kNoTarget) {
                cmd.kind = CommandKind::Follow;
                cmd.target = ally;
                cmd.x = gx;
                cmd.y = gy;
                return cmd;
            }
        }
        cmd.kind = attack_move ? CommandKind::AttackMove : CommandKind::Move;
        cmd.x = gx;
        cmd.y = gy;
    }
    return cmd;
}

std::vector<FormationTarget> formation_targets(const sim::World& world, const std::vector<uint32_t>& selection,
                                               sim::Fixed tx, sim::Fixed ty, float mixed_formation_diameter,
                                               float air_formation_diameter) {
    std::vector<FormationTarget> out;
    std::vector<uint32_t> live;
    Fixed minx, miny, maxx, maxy, sx, sy;
    for (uint32_t id : selection) {
        if (id >= world.units().size() || !world.units()[id].alive) continue;
        const sim::Unit& u = world.units()[id];
        if (live.empty()) { minx = maxx = u.x; miny = maxy = u.y; }
        minx = u.x < minx ? u.x : minx;
        maxx = u.x > maxx ? u.x : maxx;
        miny = u.y < miny ? u.y : miny;
        maxy = u.y > maxy ? u.y : maxy;
        sx = sx + u.x;
        sy = sy + u.y;
        live.push_back(id);
    }
    if (live.empty()) return out;
    const bool inside = tx >= minx && tx <= maxx && ty >= miny && ty <= maxy;
    const Fixed cx = sx / Fixed::from_int(int32_t(live.size())), cy = sy / Fixed::from_int(int32_t(live.size()));
    bool all_air = true;
    Fixed max_d2;
    for (uint32_t id : live) {
        const sim::Unit& u = world.units()[id];
        if (!u.flying) all_air = false;
        const Fixed dx = u.x - cx, dy = u.y - cy, d2 = dx * dx + dy * dy;
        if (d2 > max_d2) max_d2 = d2;
    }
    const Fixed half = Fixed::from_double(0.5 * double(all_air ? air_formation_diameter : mixed_formation_diameter));
    const bool fits = max_d2 <= half * half;
    for (uint32_t id : live) {
        const sim::Unit& u = world.units()[id];
        FormationTarget t;
        t.id = id;
        if (live.size() < 2 || inside || !fits) { t.x = tx; t.y = ty; }
        else { t.x = tx + (u.x - cx); t.y = ty + (u.y - cy); }
        out.push_back(t);
    }
    return out;
}

Command Control::issue_command(sim::World& world,
                               const std::vector<uint32_t>& selection,
                               const ScreenPoint& p, bool attack_move, bool queued) const {
    Command cmd = resolve_command(world, p, attack_move);
    if (cmd.kind == CommandKind::None) return cmd;

    if (cmd.kind == CommandKind::Gather) {
        for (uint32_t id : selection) {
            if (id >= world.units().size()) continue;
            sim::Unit& u = world.unit(id);
            if (u.is_worker) {
                u.gather_target = cmd.target;
                if (u.home_depot >= world.units().size()) {
                    uint32_t hall = kNoTarget;
                    Fixed bd;
                    for (const sim::Unit& o : world.units()) {
                        if (!o.alive || o.player != player_ || o.is_worker ||
                            o.resource_left > 0 || o.id == id)
                            continue;
                        Fixed dx = o.x - u.x, dy = o.y - u.y, d = dx * dx + dy * dy;
                        if (hall == kNoTarget || d < bd) { hall = o.id; bd = d; }
                    }
                    if (hall != kNoTarget) u.home_depot = hall;
                }
                world.issue(id, Order{OrderType::Gather, Fixed(), Fixed(), kNoTarget});
            } else {
                world.issue(id, world.smart_order(id, cmd.target, cmd.x, cmd.y), queued);
            }
        }
        return cmd;
    }

    if (cmd.kind == CommandKind::AttackMove || cmd.kind == CommandKind::Move) {
        for (const FormationTarget& t : formation_targets(world, selection, cmd.x, cmd.y)) {
            Order order;
            order.type = cmd.kind == CommandKind::AttackMove ? OrderType::AttackMove : OrderType::Move;
            order.tx = t.x;
            order.ty = t.y;
            world.issue(t.id, order, queued);
        }
        return cmd;
    }
    for (uint32_t id : selection)
        world.issue(id, world.smart_order(id, cmd.target, cmd.x, cmd.y), queued);
    return cmd;
}

}
