#include "sc2/render/offscreen.hpp"

#include "sc2/render/scene.hpp"

namespace sc2::render {

RenderResult render_model(const formats::Model& model, uint32_t width,
                          uint32_t height, const casc::Storage* storage,
                          const std::string& texture_root, const float* team_rgb,
                          int seq, float time_ms) {
    SceneInstance inst;
    inst.model = &model;
    inst.world = Mat4::identity();
    if (team_rgb)
        inst.team = {team_rgb[0], team_rgb[1], team_rgb[2], 1.0f};
    else
        inst.team = {0, 0, 0, -1.0f};
    inst.seq = seq;
    inst.time_ms = time_ms;
    return render_scene({inst}, width, height, nullptr, storage, texture_root);
}

}
