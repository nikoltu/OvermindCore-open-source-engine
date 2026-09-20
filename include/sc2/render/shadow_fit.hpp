#pragma once

#include <algorithm>
#include <cmath>

#include "sc2/render/mat4.hpp"
#include "sc2/render/scene.hpp"

namespace sc2::render {

struct ShadowBox {
    float xmn = 0, xmx = 0, ymn = 0, ymx = 0, znear = 0, zfar = 0;
    Vec3 eye{0, 0, 0};
    Vec3 center{0, 0, 0};
    Vec3 up{0, 0, 1};
    float span() const { return std::max(xmx - xmn, ymx - ymn); }
    float texel(uint32_t dim) const { return dim ? span() / float(dim) : 0.0f; }
};

inline ShadowBox fit_shadow_ortho(const Vec3& aabb_min, const Vec3& aabb_max, const Camera& cam,
                                  float aspect, const Vec3& light_dir, float pad, uint32_t dim,
                                  float shadow_clip, bool camera_fit) {
    ShadowBox out;
    const Vec3 wmn = aabb_min, wmx = aabb_max;
    const Vec3 sc{(wmn.x + wmx.x) * 0.5f, (wmn.y + wmx.y) * 0.5f, (wmn.z + wmx.z) * 0.5f};
    float sradius = 0.5f * std::sqrt(dot(sub(wmx, wmn), sub(wmx, wmn)));
    if (sradius < 1e-4f) sradius = 1.0f;
    const Vec3 L = light_dir;
    out.center = sc;
    out.eye = {sc.x - L.x * sradius * 2.0f, sc.y - L.y * sradius * 2.0f, sc.z - L.z * sradius * 2.0f};
    out.up = std::fabs(L.z) > 0.99f ? Vec3{0, 1, 0} : Vec3{0, 0, 1};
    const Mat4 lview = look_at(out.eye, out.center, out.up);

    auto to_light = [&](const Vec3& p) {
        return Vec3{lview.m[0] * p.x + lview.m[4] * p.y + lview.m[8] * p.z + lview.m[12],
                    lview.m[1] * p.x + lview.m[5] * p.y + lview.m[9] * p.z + lview.m[13],
                    lview.m[2] * p.x + lview.m[6] * p.y + lview.m[10] * p.z + lview.m[14]};
    };

    float xmn = 1e30f, xmx = -1e30f, ymn = 1e30f, ymx = -1e30f, zmn = 1e30f, zmx = -1e30f;
    auto add = [&](const Vec3& w) {
        const Vec3 v = to_light(w);
        xmn = std::min(xmn, v.x); xmx = std::max(xmx, v.x);
        ymn = std::min(ymn, v.y); ymx = std::max(ymx, v.y);
        zmn = std::min(zmn, v.z); zmx = std::max(zmx, v.z);
    };

    for (int cx = 0; cx < 2; ++cx)
        for (int cy = 0; cy < 2; ++cy)
            for (int cz = 0; cz < 2; ++cz)
                add(Vec3{cx ? wmx.x : wmn.x, cy ? wmx.y : wmn.y, cz ? wmx.z : wmn.z});
    const float depth_zmn = zmn, depth_zmx = zmx;

    bool narrowed = false;
    if (camera_fit) {
        const Vec3 fwd = normalize(sub(cam.center, cam.eye));
        Vec3 right = cross(fwd, cam.up);
        if (dot(right, right) < 1e-12f) right = Vec3{1, 0, 0};
        right = normalize(right);
        const Vec3 upv = normalize(cross(right, fwd));
        const float th = std::tan(cam.fovy * 0.5f);
        const float tw = th * aspect;
        float cap = shadow_clip > 0.0f
                        ? shadow_clip
                        : std::sqrt(dot(sub(sc, cam.eye), sub(sc, cam.eye))) + sradius;
        if (cap < 1.0f) cap = 1.0f;
        float fxmn = 1e30f, fxmx = -1e30f, fymn = 1e30f, fymx = -1e30f;
        bool any = false;
        for (int sx = -1; sx <= 1; sx += 2)
            for (int sy = -1; sy <= 1; sy += 2) {
                const Vec3 d = normalize(Vec3{fwd.x + right.x * tw * float(sx) + upv.x * th * float(sy),
                                              fwd.y + right.y * tw * float(sx) + upv.y * th * float(sy),
                                              fwd.z + right.z * tw * float(sx) + upv.z * th * float(sy)});
                float t0 = cam.znear, t1 = cap;
                if (std::fabs(d.z) > 1e-6f) {
                    float ta = (wmn.z - cam.eye.z) / d.z;
                    float tb = (wmx.z - cam.eye.z) / d.z;
                    if (ta > tb) std::swap(ta, tb);
                    t0 = std::max(t0, ta);
                    t1 = std::min(t1, tb);
                    if (t1 < t0) continue;
                }
                t1 = std::min(t1, cap);
                for (float t : {t0, t1}) {
                    const Vec3 w{cam.eye.x + d.x * t, cam.eye.y + d.y * t, cam.eye.z + d.z * t};
                    const Vec3 v = to_light(w);
                    fxmn = std::min(fxmn, v.x); fxmx = std::max(fxmx, v.x);
                    fymn = std::min(fymn, v.y); fymx = std::max(fymx, v.y);
                    any = true;
                }
            }
        if (any && fxmx > fxmn && fymx > fymn) {
            const float lh = std::sqrt(L.x * L.x + L.y * L.y);
            const float reach = std::fabs(L.z) > 1e-4f ? (wmx.z - wmn.z) * lh / std::fabs(L.z) : 0.0f;
            const float grow = std::min(reach, sradius);
            xmn = std::max(xmn, fxmn - grow); xmx = std::min(xmx, fxmx + grow);
            ymn = std::max(ymn, fymn - grow); ymx = std::min(ymx, fymx + grow);
            narrowed = xmx > xmn && ymx > ymn;
        }
    }
    if (!narrowed) {
        xmn = 1e30f; xmx = -1e30f; ymn = 1e30f; ymx = -1e30f;
        for (int cx = 0; cx < 2; ++cx)
            for (int cy = 0; cy < 2; ++cy)
                for (int cz = 0; cz < 2; ++cz) {
                    const Vec3 v = to_light(Vec3{cx ? wmx.x : wmn.x, cy ? wmx.y : wmn.y,
                                                 cz ? wmx.z : wmn.z});
                    xmn = std::min(xmn, v.x); xmx = std::max(xmx, v.x);
                    ymn = std::min(ymn, v.y); ymx = std::max(ymx, v.y);
                }
    }

    out.xmn = xmn - pad; out.xmx = xmx + pad;
    out.ymn = ymn - pad; out.ymx = ymx + pad;

    if (narrowed && dim) {
        const float tx = (out.xmx - out.xmn) / float(dim);
        const float ty = (out.ymx - out.ymn) / float(dim);
        if (tx > 0.0f) { const float s = std::floor(out.xmn / tx) * tx - out.xmn;
                         out.xmn += s; out.xmx += s; }
        if (ty > 0.0f) { const float s = std::floor(out.ymn / ty) * ty - out.ymn;
                         out.ymn += s; out.ymx += s; }
    }

    out.znear = std::max(0.001f, -depth_zmx - pad);
    out.zfar = -depth_zmn + pad;
    return out;
}

}
