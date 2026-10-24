#pragma once

#include <cstdint>
#include <vector>

#include "sc2/formats/m3.hpp"
#include "sc2/render/mat4.hpp"

namespace sc2::casc {
class Storage;
}

namespace sc2::render {

struct RenderResult {
    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<uint8_t> rgba;
    Mat4 mvp;
    uint32_t lit_drawn = 0, lit_culled = 0;
};

RenderResult render_model(const formats::Model& model, uint32_t width,
                          uint32_t height, const casc::Storage* storage = nullptr,
                          const std::string& texture_root = "",
                          const float* team_rgb = nullptr, int seq = -1,
                          float time_ms = 0.0f);

bool project(const Mat4& mvp, float x, float y, float z, uint32_t width,
             uint32_t height, float& px, float& py);

inline bool& offscreen_on_window_device() {
    static bool v = true;
    return v;
}

}
