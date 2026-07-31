#include "sc2/app/control.hpp"

#include <algorithm>
#include <cmath>

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

std::vector<uint32_t> Control::box_select(const sim::World& world,
                                          const ScreenRect& rect) const {
    float minx = std::min(rect.x0, rect.x1), maxx = std::max(rect.x0, rect.x1);
    float miny = std::min(rect.y0, rect.y1), maxy = std::max(rect.y0, rect.y1);

    std::vector<uint32_t> hits;
    for (const sim::Unit& u : world.units()) {
        if (!u.alive || u.player != player_) continue;
        const float ux = float(u.x.to_double()), uy = float(u.y.to_double());
        float fx, fy;
        if (!project(ux, uy, 0.0f, fx, fy)) continue;
        float xlo = fx, xhi = fx, ylo = fy, yhi = fy;
        float tx, ty;
        if (project(ux, uy, float(u.radius.to_double()) * kSelectHeightPerRadius, tx, ty)) {
            xlo = std::min(fx, tx); xhi = std::max(fx, tx);
            ylo = std::min(fy, ty); yhi = std::max(fy, ty);
        }
        if (xhi >= minx && xlo <= maxx && yhi >= miny && ylo <= maxy) hits.push_back(u.id);
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
        if (!u.alive || u.player == player_) continue;
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
    Fixed gx, gy;
    if (ground_point(world, p, gx, gy)) {
        cmd.kind = attack_move ? CommandKind::AttackMove : CommandKind::Move;
        cmd.x = gx;
        cmd.y = gy;
    }
    return cmd;
}

Command Control::issue_command(sim::World& world,
                               const std::vector<uint32_t>& selection,
                               const ScreenPoint& p, bool attack_move) const {
    Command cmd = resolve_command(world, p, attack_move);
    if (cmd.kind == CommandKind::None) return cmd;

    Order order;
    switch (cmd.kind) {
        case CommandKind::Attack:
            order.type = OrderType::Attack;
            order.target = cmd.target;
            break;
        case CommandKind::Move:
            order.type = OrderType::Move;
            order.tx = cmd.x;
            order.ty = cmd.y;
            break;
        case CommandKind::AttackMove:
            order.type = OrderType::AttackMove;
            order.tx = cmd.x;
            order.ty = cmd.y;
            break;
        case CommandKind::None:
            return cmd;
    }
    for (uint32_t id : selection) world.issue(id, order);
    return cmd;
}

}
