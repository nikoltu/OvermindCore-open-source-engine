#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "sc2/formats/m3.hpp"
#include "sc2/render/mat4.hpp"
#include "sc2/render/offscreen.hpp"

namespace sc2::casc {
class Storage;
}

namespace sc2::render {

struct SceneInstance {
    const formats::Model* model = nullptr;
    Mat4 world = Mat4::identity();
    std::array<float, 4> team = {0, 0, 0, -1};
    int seq = -1;
    float time_ms = 0.0f;
};

struct Camera {
    Vec3 eye{0, 0, 0};
    Vec3 center{0, 0, 0};
    Vec3 up{0, 0, 1};
    float fovy = 0.7854f;
    float znear = 0.1f;
    float zfar = 1000.0f;
    bool mirror_x = false;

    bool orthographic = false;
    float ortho_half_h = 1.0f;

    Mat4 view_proj(float aspect) const {
        Mat4 p = orthographic ? ortho(-ortho_half_h * aspect, ortho_half_h * aspect, -ortho_half_h,
                                      ortho_half_h, znear, zfar)
                              : perspective(fovy, aspect, znear, zfar);
        if (mirror_x) p.m[0] = -p.m[0];
        return p * look_at(eye, center, up);
    }
};

RenderResult render_scene(const std::vector<SceneInstance>& instances,
                          uint32_t width, uint32_t height,
                          const Camera* cam = nullptr,
                          const casc::Storage* storage = nullptr,
                          const std::string& texture_root = "",
                          bool enable_particles = false);

}
