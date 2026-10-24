#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "sc2/render/anim.hpp"
#include "sc2/render/fog_field.hpp"
#include "sc2/formats/m3.hpp"
#include "sc2/render/mat4.hpp"
#include "sc2/render/offscreen.hpp"

namespace sc2::casc {
class Storage;
}

namespace sc2::data { struct EnvLight; }

namespace sc2::render {

enum class FxKind : std::uint32_t { None = 0, Cloak = 1, Dissolve = 2, GroundClip = 3, Highlight = 4, Flash = 5, Water = 6, Creep = 7 };
struct InstanceFX {
    FxKind kind = FxKind::None;
    float t = 0.0f;
    float p0[4] = {0, 0, 0, 0};
    float p1[4] = {0, 0, 0, 0};
    float aux = 0.0f;
    float ext[2] = {0.0f, 0.0f};
    float uv_angle = 0.0f;
    float uv_tiling[2] = {1.0f, 1.0f};
    float team_emissive[4] = {0, 0, 0, 0};
    float mask_x[4] = {1.0f, 1.0f, 0.0f, 0.0f};
    float mask2_x[4] = {1.0f, 1.0f, 0.0f, 0.0f};
    float mask2_lv[4] = {1.0f, 0.0f, 0.0f, 0.0f};
    float mask_o[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    float emis2[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    float emis2_t[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    float emis_uv[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    float fres_d[4] = {0, 0, 0, 0};
    float fres_m[4] = {0, 0, 0, 0};
    float fres_m2[4] = {0, 0, 0, 0};
    float fres_s[4] = {0, 0, 0, 0};
    float fres_e[4] = {0, 0, 0, 0};
    float fres_em[4] = {0, 0, 0, 0};
    float uv_src = 0.0f;
    bool is_default() const {
        return kind == FxKind::None && uv_angle == 0.0f && uv_tiling[0] == 1.0f && uv_tiling[1] == 1.0f &&
               team_emissive[3] == 0.0f;
    }
};
struct FxRecord {
    float head[4];
    float p0[4];
    float p1[4];
    float uv[4];
    float team_emissive[4];
    float mask_x[4];
    float mask2_x[4];
    float mask2_lv[4];
    float fres_d[4];
    float fres_m[4];
    float fres_m2[4];
    float fres_s[4];
    float fres_e[4];
    float fres_em[4];
    float mask_o[4];
    float emis2[4];
    float emis2_t[4];
    float emis_uv[4];
};
inline FxRecord fx_record_of(const InstanceFX& f) {
    FxRecord r{};
    r.head[0] = float(std::uint32_t(f.kind)) + f.uv_src;
    r.head[1] = f.t;
    r.head[2] = f.uv_angle;
    r.head[3] = f.aux;
    r.uv[2] = f.ext[0];
    r.uv[3] = f.ext[1];
    for (int k = 0; k < 4; ++k) { r.p0[k] = f.p0[k]; r.p1[k] = f.p1[k]; r.team_emissive[k] = f.team_emissive[k]; }
    for (int k = 0; k < 4; ++k) { r.mask_x[k] = f.mask_x[k]; r.mask2_x[k] = f.mask2_x[k]; r.mask2_lv[k] = f.mask2_lv[k]; }
    for (int k = 0; k < 4; ++k) { r.fres_d[k] = f.fres_d[k]; r.fres_m[k] = f.fres_m[k]; r.fres_m2[k] = f.fres_m2[k]; }
    for (int k = 0; k < 4; ++k) { r.fres_s[k] = f.fres_s[k]; r.fres_e[k] = f.fres_e[k]; r.fres_em[k] = f.fres_em[k]; }
    for (int k = 0; k < 4; ++k) { r.mask_o[k] = f.mask_o[k]; r.emis2[k] = f.emis2[k]; r.emis2_t[k] = f.emis2_t[k]; r.emis_uv[k] = f.emis_uv[k]; }
    r.uv[0] = f.uv_tiling[0];
    r.uv[1] = f.uv_tiling[1];
    return r;
}

struct SceneInstance {
    const formats::Model* model = nullptr;
    Mat4 world = Mat4::identity();
    std::array<float, 4> team = {0, 0, 0, -1};
    std::array<float, 4> team_emissive = {0, 0, 0, -1};
    int seq = -1;
    float time_ms = 0.0f;
    std::uint64_t mesh_version = 0;
    float opacity = 1.0f;
    std::array<float, 3> tint = {1.0f, 1.0f, 1.0f};
    int blend_seq = -1;
    float blend_time_ms = 0.0f;
    float blend_weight = 0.0f;
    ClipLayers global_layers;
    int turret_bone = -1;
    float turret_yaw = 0.0f;
    InstanceFX fx;
    int beam_bone = -1;
    std::array<float, 3> beam_target = {0.0f, 0.0f, 0.0f};
    int reach_tip_bone = -1, reach_root_bone = -1, reach_weapon_bone = -1;
    std::array<float, 3> reach_target = {0.0f, 0.0f, 0.0f};
    const FogField* coverage = nullptr;
    std::uint32_t trail_id = 0;
    float trail_time_s = 0.0f;
    Vec3 path_from{0, 0, 0};
    float path_speed = 0.0f;
    float path_ended_s = 0.0f;
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
    float ortho_half_w = 0.0f;
    bool transparent_clear = false;
    float dof_focus = 0.0f, dof_start = 0.0f, dof_end = 0.0f, dof_amount = 0.0f, dof_radius_px = 0.0f;

    Mat4 view_proj(float aspect) const {
        const float hw = ortho_half_w > 0.0f ? ortho_half_w : ortho_half_h * aspect;
        Mat4 p = orthographic ? ortho(-hw, hw, -ortho_half_h, ortho_half_h, znear, zfar)
                              : perspective(fovy, aspect, znear, zfar);
        if (orthographic) p.m[5] = -p.m[5];
        if (mirror_x) p.m[0] = -p.m[0];
        return p * look_at(eye, center, up);
    }
};

RenderResult render_scene_session(const std::string& key, const std::vector<SceneInstance>& instances, uint32_t width, uint32_t height,
                                  const Camera& cam, const casc::Storage* storage, const data::EnvLight* env,
                                  std::size_t preload_budget = 0, bool* preload_done = nullptr);
bool render_scene_session_exists(const std::string& key, uint32_t width, uint32_t height);
void release_scene_sessions();
bool request_model_textures(const formats::Model& m, const casc::Storage* storage, const std::string& texture_root, uint32_t max_dim);
uint32_t render_scene_session_texture_max();

RenderResult render_scene(const std::vector<SceneInstance>& instances,
                          uint32_t width, uint32_t height,
                          const Camera* cam = nullptr,
                          const casc::Storage* storage = nullptr,
                          const std::string& texture_root = "",
                          bool enable_particles = false,
                          const data::EnvLight* env = nullptr,
                          const FogField* fog = nullptr,
                          uint32_t resize_from = 0,
                          const float* window_shadow_clip = nullptr);

}
