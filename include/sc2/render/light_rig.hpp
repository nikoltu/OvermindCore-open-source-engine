#pragma once
#include <cstdlib>

#include <algorithm>
#include <cmath>

#include "sc2/data/environment.hpp"
#include "sc2/render/mat4.hpp"

namespace sc2::render {

struct RigLight {
    bool present = false;
    Vec3 dir{0, 0, -1};
    float color[3] = {0, 0, 0};
    float spec[3] = {0, 0, 0};
};

struct LightRig {
    bool present = false;
    float ambient[3] = {0, 0, 0};
    RigLight key, fill, back;
};

inline LightRig rig_from_env(const data::EnvLight& env) {
    LightRig r;
    if (!env.found) return r;
    auto one = [](const data::EnvDirLight& s, RigLight& d) {
        if (!s.present) return;
        const Vec3 v{float(s.direction[0]), float(s.direction[1]), float(s.direction[2])};
        if (v.x * v.x + v.y * v.y + v.z * v.z <= 1e-9f) return;
        d.present = true;
        d.dir = normalize(v);
        for (int i = 0; i < 3; ++i) {
            d.color[i] = float(s.color[i] * s.color_multiplier);
            d.spec[i] = float(s.spec_color[i] * s.spec_multiplier);
        }
    };
    if (!std::getenv("SC2_NO_KEY")) one(env.key, r.key);
    one(env.fill, r.fill);
    one(env.back, r.back);
    if (env.has_ambient)
        for (int i = 0; i < 3; ++i) r.ambient[i] = float(env.ambient_color[i]);
    r.present = r.key.present || r.fill.present || r.back.present;
    return r;
}

inline void shade_retail(const LightRig& r, const Vec3& n, float sh, float out[3], float ambient_mult = 1.0f,
                         float diffuse_mult = 1.0f) {
    for (int i = 0; i < 3; ++i) out[i] = r.ambient[i] * ambient_mult;
    const RigLight* ls[3] = {&r.key, &r.fill, &r.back};
    for (int k = 0; k < 3; ++k) {
        const RigLight* l = ls[k];
        if (!l->present) continue;
        const float d = std::max(0.0f, -(n.x * l->dir.x + n.y * l->dir.y + n.z * l->dir.z));
        for (int i = 0; i < 3; ++i) out[i] += l->color[i] * diffuse_mult * d * (k == 0 ? sh : 1.0f);
    }
}

inline void shade(const LightRig& r, const Vec3& n, float sh, float out[3], float floor_ = 0.0f) {
    for (int i = 0; i < 3; ++i) out[i] = r.ambient[i];
    const RigLight* ls[3] = {&r.key, &r.fill, &r.back};
    for (const RigLight* l : ls) {
        if (!l->present) continue;
        const float d = std::max(0.0f, -(n.x * l->dir.x + n.y * l->dir.y + n.z * l->dir.z));
        if (d <= 0.0f) continue;
        for (int i = 0; i < 3; ++i) out[i] += l->color[i] * d;
    }
    const float f = floor_ + (1.0f - floor_) * sh;
    for (int i = 0; i < 3; ++i) out[i] *= f;
}

}
