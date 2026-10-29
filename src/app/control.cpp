#include "omc/app/control.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <string>

#include "omc/sim/weapon.hpp"

namespace omc::app {

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

bool invert_mat4(const render::Mat4& m, render::Mat4& out) { return invert4(m.m, out.m); }

bool Control::view_ray(const ScreenPoint& p, float o[3], float d[3]) const {
    if (!invertible_ || view_w_ == 0 || view_h_ == 0) return false;
    const float ndc_x = (p.x / float(view_w_)) * 2.0f - 1.0f;
    const float ndc_y = (p.y / float(view_h_)) * 2.0f - 1.0f;
    float n[4], f[4];
    transform_point(inv_mvp_.m, ndc_x, ndc_y, 0.0f, n);
    transform_point(inv_mvp_.m, ndc_x, ndc_y, 1.0f, f);
    if (std::fabs(n[3]) < 1e-12f || std::fabs(f[3]) < 1e-12f) return false;
    for (int i = 0; i < 3; ++i) {
        o[i] = n[i] / n[3];
        d[i] = f[i] / f[3] - o[i];
    }
    return true;
}

static float capsule_t(const float lo[3], const float ld[3], float r, float h);

float ray_volume_t(const PickVolume& v, const float o[3], const float d[3]) {
    const auto& m = v.inv.m;
    const float lo[3] = {m[0] * o[0] + m[4] * o[1] + m[8] * o[2] + m[12], m[1] * o[0] + m[5] * o[1] + m[9] * o[2] + m[13],
                         m[2] * o[0] + m[6] * o[1] + m[10] * o[2] + m[14]};
    const float ld[3] = {m[0] * d[0] + m[4] * d[1] + m[8] * d[2], m[1] * d[0] + m[5] * d[1] + m[9] * d[2],
                         m[2] * d[0] + m[6] * d[1] + m[10] * d[2]};
    auto sphere = [&](const float c[3], float r) -> float {
        const float q[3] = {lo[0] - c[0], lo[1] - c[1], lo[2] - c[2]};
        const float a = ld[0] * ld[0] + ld[1] * ld[1] + ld[2] * ld[2];
        const float b = q[0] * ld[0] + q[1] * ld[1] + q[2] * ld[2];
        const float cc = q[0] * q[0] + q[1] * q[1] + q[2] * q[2] - r * r;
        const float disc = b * b - a * cc;
        if (a <= 0.0f || disc < 0.0f) return -1.0f;
        const float t = (-b - std::sqrt(disc)) / a;
        return t >= 0.0f ? t : (cc <= 0.0f ? 0.0f : -1.0f);
    };
    if (v.shape == 0) {
        float t0 = 0.0f, t1 = 1e30f;
        for (int i = 0; i < 3; ++i) {
            const float h = v.size[i];
            if (std::fabs(ld[i]) < 1e-12f) {
                if (lo[i] < -h || lo[i] > h) return -1.0f;
                continue;
            }
            float a = (-h - lo[i]) / ld[i], b = (h - lo[i]) / ld[i];
            if (a > b) std::swap(a, b);
            t0 = std::max(t0, a);
            t1 = std::min(t1, b);
            if (t0 > t1) return -1.0f;
        }
        return t0;
    }
    const float r = v.size[0];
    if (v.shape == 1) {
        const float c[3] = {0, 0, 0};
        return sphere(c, r);
    }
    float h = 0.5f * v.size[1];
    static const char* cap = std::getenv("OMC_659_CAPSULE");
    if (cap && cap[0] == 'h') h = v.size[1];
    float lo_c[3] = {lo[0], lo[1], lo[2]}, ld_c[3] = {ld[0], ld[1], ld[2]};
    if (cap && (cap[0] == 'x' || cap[0] == 'y')) {
        const int a = cap[0] == 'x' ? 0 : 1;
        std::swap(lo_c[a], lo_c[2]);
        std::swap(ld_c[a], ld_c[2]);
    }
    return capsule_t(lo_c, ld_c, r, h);
}

static float capsule_t(const float lo[3], const float ld[3], float r, float h) {
    auto sphere = [&](const float c[3]) -> float {
        const float q[3] = {lo[0] - c[0], lo[1] - c[1], lo[2] - c[2]};
        const float a = ld[0] * ld[0] + ld[1] * ld[1] + ld[2] * ld[2];
        const float b = q[0] * ld[0] + q[1] * ld[1] + q[2] * ld[2];
        const float cc = q[0] * q[0] + q[1] * q[1] + q[2] * q[2] - r * r;
        const float disc = b * b - a * cc;
        if (a <= 0.0f || disc < 0.0f) return -1.0f;
        const float t = (-b - std::sqrt(disc)) / a;
        return t >= 0.0f ? t : (cc <= 0.0f ? 0.0f : -1.0f);
    };
    float best = -1.0f;
    const float a = ld[0] * ld[0] + ld[1] * ld[1];
    if (a > 1e-12f) {
        const float b = lo[0] * ld[0] + lo[1] * ld[1];
        const float c = lo[0] * lo[0] + lo[1] * lo[1] - r * r;
        const float disc = b * b - a * c;
        if (disc >= 0.0f) {
            float t = (-b - std::sqrt(disc)) / a;
            if (t < 0.0f && c <= 0.0f) t = 0.0f;
            const float z = lo[2] + t * ld[2];
            if (t >= 0.0f && z >= -h && z <= h) best = t;
        }
    }
    for (int s = -1; s <= 1; s += 2) {
        const float c[3] = {0, 0, float(s) * h};
        const float t = sphere(c);
        if (t >= 0.0f && (best < 0.0f || t < best)) best = t;
    }
    return best;
}

std::vector<Control::RayHit> Control::ray_hits(const sim::World& world, const ScreenPoint& p) const {
    std::vector<RayHit> out;
    float o[3], d[3];
    if (!volumes_ || !view_ray(p, o, d)) return out;
    std::vector<PickVolume> vols;
    for (const sim::Unit& u : world.units()) {
        if (!u.alive || u.unselectable || u.loaded || u.in_gas_structure) continue;
        if (!volumes_->may_hit(u.id, o, d)) continue;
        vols.clear();
        if (!volumes_->volumes(u.id, vols)) continue;
        RayHit h;
        h.id = u.id;
        bool any = false;
        for (const PickVolume& v : vols) {
            const float t = ray_volume_t(v, o, d);
            if (t < 0.0f) continue;
            if (!any || t < h.t) h.t = t;
            any = true;
            if (v.tight && (!h.tight || t < h.tight_t)) {
                h.tight = true;
                h.tight_t = t;
            }
            if (!v.tight && (!h.fuzzy || t < h.fuzzy_t)) {
                h.fuzzy = true;
                h.fuzzy_t = t;
            }
        }
        if (any) out.push_back(h);
    }
    return out;
}

std::uint32_t Control::click_pick(const sim::World& world, const ScreenPoint& p, ClickRule rule) const {
    std::uint32_t best = kNoTarget;
    float best_t = 0.0f;
    int best_rank = 0;
    for (const RayHit& h : ray_hits(world, p)) {
        const sim::Unit& u = world.units()[h.id];
        if (u.player != player_ && (u.cloaked || u.burrowed) && !world.revealed_to(player_, u.id)) continue;
        int rank = 0;
        float t = h.t;
        if (rule == ClickRule::TightFirst) {
            rank = h.tight ? 0 : 1;
            if (h.tight) t = h.tight_t;
        } else if (rule == ClickRule::OwnFirst) {
            rank = u.player == player_ ? 0 : 1;
        } else if (rule == ClickRule::TightOnly) {
            if (!h.tight) continue;
            t = h.tight_t;
        } else if (rule == ClickRule::FuzzyOnly) {
            if (!h.fuzzy) continue;
            t = h.fuzzy_t;
        }
        if (best == kNoTarget || rank < best_rank || (rank == best_rank && t < best_t)) {
            best = h.id;
            best_t = t;
            best_rank = rank;
        }
    }
    return best;
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

bool Control::volume_screen_rect(std::uint32_t id, ScreenRect& r, int which) const {
    if (!volumes_) return false;
    std::vector<PickVolume> vols;
    if (!volumes_->volumes(id, vols) || vols.empty()) return false;
    if (which == 1 && std::none_of(vols.begin(), vols.end(), [](const PickVolume& v) { return v.tight; })) which = 0;
    static const std::vector<std::array<float, 3>> dirs = [] {
        std::vector<std::array<float, 3>> d;
        d.push_back({0.0f, 0.0f, 1.0f});
        d.push_back({0.0f, 0.0f, -1.0f});
        for (int a = 1; a <= 8; ++a) {
            const float th = 3.14159265f * float(a) / 9.0f;
            for (int b = 0; b < 16; ++b) {
                const float ph = 6.28318531f * float(b) / 16.0f;
                d.push_back({std::sin(th) * std::cos(ph), std::sin(th) * std::sin(ph), std::cos(th)});
            }
        }
        return d;
    }();
    bool any = false;
    float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    for (const PickVolume& v : vols) {
        if ((which == 1 && !v.tight) || (which == 2 && v.tight)) continue;
        render::Mat4 fwd;
        if (!invert_mat4(v.inv, fwd)) continue;
        const auto& m = fwd.m;
        auto put = [&](float lx, float ly, float lz) {
            const float wx = m[0] * lx + m[4] * ly + m[8] * lz + m[12];
            const float wy = m[1] * lx + m[5] * ly + m[9] * lz + m[13];
            const float wz = m[2] * lx + m[6] * ly + m[10] * lz + m[14];
            float px, py;
            if (!project(wx, wy, wz, px, py)) return;
            if (!any) {
                x0 = x1 = px;
                y0 = y1 = py;
                any = true;
            } else {
                x0 = std::min(x0, px); x1 = std::max(x1, px);
                y0 = std::min(y0, py); y1 = std::max(y1, py);
            }
        };
        if (v.shape == 0) {
            for (int c = 0; c < 8; ++c)
                put(c & 1 ? v.size[0] : -v.size[0], c & 2 ? v.size[1] : -v.size[1], c & 4 ? v.size[2] : -v.size[2]);
            continue;
        }
        const float rad = v.size[0];
        const float h = v.shape == 2 ? 0.5f * v.size[1] : 0.0f;
        for (int e = (v.shape == 2 ? -1 : 0); e <= (v.shape == 2 ? 1 : 0); e += 2) {
            const float cz = float(e) * h;
            for (const auto& d : dirs) put(d[0] * rad, d[1] * rad, cz + d[2] * rad);
            if (v.shape != 2) break;
        }
    }
    if (!any) return false;
    r = ScreenRect{x0, y0, x1, y1};
    return true;
}

std::vector<uint32_t> Control::box_select_672(const sim::World& world, const ScreenRect& rect) const {
    const float minx = std::min(rect.x0, rect.x1), maxx = std::max(rect.x0, rect.x1);
    const float miny = std::min(rect.y0, rect.y1), maxy = std::max(rect.y0, rect.y1);
    struct Foreign {
        std::uint32_t id;
        bool structure;
        int rank;
        float cx;
    };
    std::vector<std::uint32_t> own_units, own_structs;
    std::vector<Foreign> foreign;
    auto rank_of = [&](const sim::Unit& u) { return box_rules_.rank ? box_rules_.rank(u) : 0; };
    for (const sim::Unit& u : world.units()) {
        if (!u.alive || u.unselectable || u.in_gas_structure) continue;
        if (u.loaded && world.cargo_hidden()) continue;
        const bool own = u.player == player_;
        if (!own && (u.cloaked || u.burrowed) && !world.revealed_to(player_, u.id)) continue;
        const float ux = float(u.x.to_double()), uy = float(u.y.to_double());
        const float fz = ground_z(ux, uy);
        float fx, fy;
        if (!project(ux, uy, fz, fx, fy)) continue;
        if (fx < minx - 400.0f || fx > maxx + 400.0f || fy < miny - 400.0f || fy > maxy + 600.0f) continue;
        ScreenRect br;
        static const char* vw = std::getenv("OMC_672_BOX_VOLUMES");
        if (!volume_screen_rect(u.id, br, vw ? std::atoi(vw) : 1)) {
            if (u.player < 0) continue;
            br = ScreenRect{fx, fy, fx, fy};
            float tx, ty;
            if (project(ux, uy, fz + float(u.radius.to_double()) * kSelectHeightPerRadius, tx, ty))
                br = ScreenRect{std::min(fx, tx), std::min(fy, ty), std::max(fx, tx), std::max(fy, ty)};
        }
        if (!(br.x1 >= minx && br.x0 <= maxx && br.y1 >= miny && br.y0 <= maxy)) continue;
        const bool structure = (u.attributes & sim::kAttrStructure) != 0;
        if (own) {
            (structure ? own_structs : own_units).push_back(u.id);
        } else {
            foreign.push_back({u.id, structure, rank_of(u), 0.5f * (br.x0 + br.x1)});
        }
    }
    auto capped = [](std::vector<std::uint32_t> v) {
        if (v.size() > kSelectionCap) v.resize(kSelectionCap);
        return v;
    };
    if (!own_units.empty()) return capped(std::move(own_units));
    if (!own_structs.empty()) {
        std::uint32_t lead = own_structs[0];
        int best = rank_of(world.units()[lead]);
        for (std::uint32_t id : own_structs) {
            const int r = rank_of(world.units()[id]);
            if (r > best) {
                best = r;
                lead = id;
            }
        }
        const std::string& type = world.units()[lead].catalog_id;
        std::vector<std::uint32_t> same;
        for (std::uint32_t id : own_structs)
            if (world.units()[id].catalog_id == type) same.push_back(id);
        return capped(std::move(same));
    }
    if (foreign.empty()) return {};
    const Foreign* pick = &foreign[0];
    for (const Foreign& f : foreign) {
        if (f.structure != pick->structure) {
            if (!f.structure) pick = &f;
            continue;
        }
        if (f.rank != pick->rank) {
            if (f.rank > pick->rank) pick = &f;
            continue;
        }
        if (f.cx < pick->cx) pick = &f;
    }
    return {pick->id};
}

std::vector<uint32_t> Control::box_select(const sim::World& world, const ScreenRect& rect,
                                          bool marquee) const {
    if (marquee && box_rules_.on) return box_select_672(world, rect);
    float minx = std::min(rect.x0, rect.x1), maxx = std::max(rect.x0, rect.x1);
    float miny = std::min(rect.y0, rect.y1), maxy = std::max(rect.y0, rect.y1);

    std::vector<uint32_t> hits;
    float best_foreign_d2 = 0.0f;
    uint32_t best_foreign = 0;
    bool have_foreign = false;
    const float cxr = 0.5f * (minx + maxx), cyr = 0.5f * (miny + maxy);
    for (const sim::Unit& u : world.units()) {
        if (!u.alive || u.unselectable) continue;
        if (u.loaded && world.cargo_hidden()) continue;
        const bool own = u.player == player_;
        if (!own && u.player < 0) continue;
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
        if (!(xhi >= minx && xlo <= maxx && yhi >= miny && ylo <= maxy)) continue;
        if (own) {
            hits.push_back(u.id);
            continue;
        }
        const float mx = 0.5f * (xlo + xhi) - cxr, my = 0.5f * (ylo + yhi) - cyr;
        const float d2 = mx * mx + my * my;
        if (!have_foreign || d2 < best_foreign_d2) {
            have_foreign = true;
            best_foreign_d2 = d2;
            best_foreign = u.id;
        }
    }
    if (hits.empty() && have_foreign) return {best_foreign};
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
        if (u.loaded && world.cargo_hidden()) continue;
        if (u.is_gas && u.player < 0 && world.gas_harvest_ordered()) continue;
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
        if (u.loaded && world.cargo_hidden()) continue;
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
        if (!u.alive || u.player != player_ || u.resource_left > 0 || u.unselectable) continue;
        if (u.loaded && world.cargo_hidden()) continue;
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
