#pragma once

#include "omc/formats/placed_objects.hpp"
#include "omc/render/mat4.hpp"

#include <cmath>
#include <cstdlib>
#include <cstring>

namespace omc::app {

inline render::Mat4 doodad_world_matrix(const formats::PlacedDoodad& d) {
    render::Mat4 tilt = render::Mat4::identity();
    if ((d.pitch != 0.0f || d.roll != 0.0f)) {
        static const char* kEnv = std::getenv("OMC_DOODAD_TILT");
        static const char* kTilt = kEnv ? kEnv : "xy++r";
        if (std::strlen(kTilt) >= 5) {
            auto about = [](char axis, float a) {
                const float s = std::sin(a * 0.5f), c = std::cos(a * 0.5f);
                return axis == 'x' ? render::from_quat(s, 0.0f, 0.0f, c) : render::from_quat(0.0f, s, 0.0f, c);
            };
            const render::Mat4 p = about(kTilt[0], kTilt[2] == '-' ? -d.pitch : d.pitch);
            const render::Mat4 r = about(kTilt[1], kTilt[3] == '-' ? -d.roll : d.roll);
            tilt = kTilt[4] == 'r' ? p * r : r * p;
        }
    }
    return render::translation(d.x, d.y, d.z) * render::rotation_z(d.rotation) * tilt *
           render::scaling(d.sx, d.sy, d.sz);
}

}
