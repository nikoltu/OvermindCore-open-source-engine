#pragma once
#include <cstdlib>
#include <string>

#include <map>

#include "sc2/data/catalog.hpp"
#include "sc2/data/environment.hpp"
#include "sc2/render/camera_follow.hpp"

namespace sc2::app {

inline std::string melee_camera_profile(const data::Catalog& cat) {
    for (const std::string& id : cat.ids("CCamera"))
        if (cat.scalar("CCamera", id, "FollowScrollLeash")) return id;
    return {};
}

inline std::string game_camera_profile(const data::Catalog& cat) {
    if (cat.find("CCamera", "Dflt")) return "Dflt";
    return melee_camera_profile(cat);
}

inline float melee_shadow_clip(const data::Catalog& cat) {
    const std::string profile = game_camera_profile(cat);
    if (profile.empty()) return 0.0f;
    const std::map<std::string, double> params = data::camera_params(cat, profile);
    const auto it = params.find("ShadowClip");
    return it == params.end() ? 0.0f : float(it->second);
}

inline render::CameraFollowParams camera_follow_params(const data::Catalog& cat, const std::string& profile) {
    render::CameraFollowParams p;
    auto num = [&](const char* field, float& out) {
        if (auto v = cat.scalar_num("CCamera", profile, field)) out = float(*v);
    };
    auto quad = [&](const char* field, float& a, float& b, float& c, float& d) {
        const auto v = cat.scalar("CCamera", profile, field);
        if (!v || v->empty()) return;
        float t[4] = {a, b, c, d};
        const char* s = v->c_str();
        for (int k = 0; k < 4 && *s; ++k) {
            char* end = nullptr;
            t[k] = std::strtof(s, &end);
            if (end == s) break;
            s = end;
            if (*s == ',') ++s;
        }
        a = t[0]; b = t[1]; c = t[2]; d = t[3];
    };
    float dummy = 0.0f;
    quad("FollowScrollLeash", p.leash_l, p.leash_b, p.leash_r, p.leash_t);
    quad("FollowOffsetUpdateBandX", p.band_x0, p.band_x1, dummy, dummy);
    quad("FollowOffsetUpdateBandY", p.band_y0, p.band_y1, dummy, dummy);
    num("FollowResetTimeoutNormal", p.timeout_normal);
    num("FollowResetTimeoutLeashed", p.timeout_leashed);
    num("FollowResetTimeoutUnleashed", p.timeout_unleashed);
    num("FollowResetJumpDistance", p.jump_distance);
    num("FollowResetJumpDelay", p.jump_delay);
    num("FollowResetDecayDuration", p.decay_duration);
    num("FollowResetDecayFactor", p.decay_factor);
    for (const core::XmlNode& n : cat.resolved("CCamera", profile))
        if (n.name == "TargetSmooth")
            if (const std::string* v = n.attr("VelocityMax")) p.velocity_max = std::strtof(v->c_str(), nullptr);
    return p;
}

}
