#include "sc2/render/offscreen.hpp"

#include "sc2/render/anim.hpp"
#include "sc2/render/scene.hpp"

#include <cstdio>
#include <cstdlib>

#include "sc2/data/environment.hpp"
#include <cmath>
#include <algorithm>
#include <vector>

#include "sc2/app/console_panel.hpp"
#include "sc2/data/catalog.hpp"
#include "sc2/render/frame_prof.hpp"
#include "sc2/render/scene_renderer.hpp"
#include "sc2/render/texture_decode.hpp"
#include "sc2/formats/m3.hpp"
#include "sc2/render/mat4.hpp"
#include "sc2/app/ui_canvas.hpp"
#include "sc2/ui/layout.hpp"

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

namespace sc2::app {

namespace {
struct ConsoleStubs {
    ui::Rect minimap{}, info{}, command{};
    bool ok = false;
};

ConsoleStubs console_stubs(casc::Storage& store, std::uint32_t canvas_w, std::uint32_t canvas_h) {
    ConsoleStubs out;
    std::vector<std::uint8_t> xml;
    try {
        xml = store.read_by_name("mods/core.sc2mod/base.sc2data/UI/Layout/UI/ConsolePanel.SC2Layout");
    } catch (...) {
        return out;
    }
    ui::Layout doc;
    try {
        doc = ui::parse_layout(xml);
    } catch (...) {
        return out;
    }
    const ui::Rect canvas{0.0f, 0.0f, float(canvas_w), float(canvas_h)};
    int found = 0;
    for (const ui::Frame& top : doc.frames)
        for (const ui::SolvedFrame& f : ui::solve_layout(top, canvas)) {
            if (f.name == "MinimapStub") { out.minimap = f.rect; ++found; }
            else if (f.name == "InfoStub") { out.info = f.rect; ++found; }
            else if (f.name == "CommandStub") { out.command = f.rect; ++found; }
        }
    out.ok = found >= 3 && out.minimap.w > 0.0f && out.info.w > 0.0f && out.command.w > 0.0f;
    return out;
}

void blit_panel(std::vector<std::uint8_t>& dst, int dst_w, int dst_h,
                const render::RenderResult& src, const ui::Rect& at) {
    const int ax = int(at.x + 0.5f), ay = int(at.y + 0.5f);
    for (int y = 0; y < int(src.height); ++y) {
        const int dy = ay + y;
        if (dy < 0 || dy >= dst_h) continue;
        for (int x = 0; x < int(src.width); ++x) {
            const int dx = ax + x;
            if (dx < 0 || dx >= dst_w) continue;
            const std::size_t si = (std::size_t(y) * src.width + std::size_t(x)) * 4;
            const std::size_t di = (std::size_t(dy) * std::size_t(dst_w) + std::size_t(dx)) * 4;
            const unsigned a = src.rgba[si + 3];
            if (!a) continue;
            if (a == 255) {
                for (int k = 0; k < 4; ++k) dst[di + k] = src.rgba[si + k];
                continue;
            }
            for (int k = 0; k < 3; ++k)
                dst[di + k] = std::uint8_t((src.rgba[si + k] * a + dst[di + k] * (255 - a)) / 255);
            dst[di + 3] = std::uint8_t(a + (dst[di + 3] * (255 - a)) / 255);
        }
    }
}
}

std::vector<std::uint8_t> render_portrait(casc::Storage& store, const formats::Model& model, float fov_rad,
                                          int cam_bone, float znear, float zfar, std::uint32_t w, std::uint32_t h,
                                          const data::EnvLight* env) {
    if (!w || !h || model.vertex_count == 0) return {};
    render::SceneInstance inst;
    inst.model = &model;
    inst.world = render::Mat4::identity();
    inst.team = {0, 0, 0, -1.0f};
    inst.seq = model.sequences.empty() ? -1 : 0;
    for (std::size_t q = 0; q < model.sequences.size(); ++q)
        if (model.sequences[q].name == "Stand") { inst.seq = int(q); break; }
    if (const char* want = std::getenv("SC2_PORTRAIT_SEQ"))
        for (std::size_t q = 0; q < model.sequences.size(); ++q)
            if (model.sequences[q].name == want) { inst.seq = int(q); break; }
    inst.time_ms = 0.0f;
    if (inst.seq >= 0 && portrait_time_ms() > 0.0f) {
        const float len = float(model.sequences[std::size_t(inst.seq)].length_ms());
        if (len > 1.0f) inst.time_ms = std::fmod(portrait_time_ms(), len);
    }
    const float fov = fov_rad > 0.01f && fov_rad < 3.0f ? fov_rad : 0.654498f;
    render::Camera cam;
    cam.fovy = fov;
    cam.znear = znear > 0.0f ? znear : 0.01f;
    cam.zfar = zfar > cam.znear ? zfar : 100.0f;
    bool posed = false;
    if (cam_bone >= 0 && std::size_t(cam_bone) < model.bones.size()) {
        const std::vector<render::Mat4> bw = render::pose_world(model, inst.seq >= 0 ? std::uint32_t(inst.seq) : 0u, inst.time_ms);
        if (std::size_t(cam_bone) < bw.size()) {
            const render::Mat4& M = bw[std::size_t(cam_bone)];
            const render::Vec3 eye{M.m[12], M.m[13], M.m[14]};
            const render::Vec3 fwd{-M.m[8], -M.m[9], -M.m[10]};
            const render::Vec3 up{M.m[4], M.m[5], M.m[6]};
            const float fl = std::sqrt(fwd.x * fwd.x + fwd.y * fwd.y + fwd.z * fwd.z);
            const float ul = std::sqrt(up.x * up.x + up.y * up.y + up.z * up.z);
            if (fl > 1e-6f && ul > 1e-6f) {
                cam.eye = eye;
                cam.center = {eye.x + fwd.x, eye.y + fwd.y, eye.z + fwd.z};
                cam.up = {up.x / ul, up.y / ul, up.z / ul};
                posed = true;
            }
        }
    }
    if (!posed) {
        const float cx = 0.5f * (model.bounds_min[0] + model.bounds_max[0]);
        const float cy = 0.5f * (model.bounds_min[1] + model.bounds_max[1]);
        const float cz = 0.5f * (model.bounds_min[2] + model.bounds_max[2]);
        const float half = 0.5f * std::max(model.bounds_max[2] - model.bounds_min[2], (model.bounds_max[0] - model.bounds_min[0]) * float(h) / float(w));
        const float dist = std::max(0.05f, half / std::tan(fov * 0.5f)) * 1.05f;
        cam.eye = {cx, cy - dist, cz};
        cam.center = {cx, cy, cz};
        cam.up = {0.0f, 0.0f, 1.0f};
        cam.znear = 0.01f;
        cam.zfar = 100.0f;
    }
    if (const PortraitDof& pd = portrait_dof(); pd.amount > 0.0f && pd.end > pd.start && !std::getenv("SC2_NO_PORTRAIT_DOF")) {
        static const float kRadius = std::getenv("SC2_PORTRAIT_DOF_RADIUS") ? float(std::atof(std::getenv("SC2_PORTRAIT_DOF_RADIUS"))) : 3.0f;
        cam.dof_focus = pd.focus;
        cam.dof_start = pd.start;
        cam.dof_end = pd.end;
        cam.dof_amount = pd.amount;
        cam.dof_radius_px = kRadius * std::max(1.0f, pd.px_scale);
    }
    std::vector<render::SceneInstance> scene{inst};
    data::EnvLight flipped;
    if (env && env->found)
        if (const char* fl = std::getenv("SC2_PORTRAIT_LIGHT_FLIP") ? std::getenv("SC2_PORTRAIT_LIGHT_FLIP")
                             : std::getenv("SC2_PORTRAIT_RIG_TURNED") ? "rot270" : nullptr) {
            flipped = *env;
            const std::string mode = fl;
            const double sx = (mode == "x" || mode == "xy" || mode == "neg") ? -1.0 : 1.0;
            const double sy = (mode == "y" || mode == "xy" || mode == "neg") ? -1.0 : 1.0;
            const double sz = mode == "neg" ? -1.0 : 1.0;
            for (auto* l : {&flipped.key, &flipped.fill, &flipped.back}) {
                l->direction[0] *= sx;
                l->direction[1] *= sy;
                l->direction[2] *= sz;
            }
            flipped.key_dir[0] *= sx;
            flipped.key_dir[1] *= sy;
            flipped.key_dir[2] *= sz;
            if (mode == "rot90" || mode == "rot270" || mode == "yz" || mode == "zy" || mode.rfind("yaw:", 0) == 0) {
                const double yaw = mode.rfind("yaw:", 0) == 0 ? std::atof(mode.c_str() + 4) * 3.14159265358979 / 180.0 : 0.0;
                auto turn = [&](double* d) {
                    const double x = d[0], y = d[1], z = d[2];
                    if (mode.rfind("yaw:", 0) == 0) { d[0] = x * std::cos(yaw) - y * std::sin(yaw); d[1] = x * std::sin(yaw) + y * std::cos(yaw); }
                    else if (mode == "rot90") { d[0] = -y; d[1] = x; }
                    else if (mode == "rot270") { d[0] = y; d[1] = -x; }
                    else if (mode == "yz") { d[1] = -z; d[2] = y; }
                    else { d[1] = z; d[2] = -y; }
                };
                for (auto* l : {&flipped.key, &flipped.fill, &flipped.back}) turn(l->direction);
                turn(flipped.key_dir);
            }
            env = &flipped;
        }
    data::EnvLight scaled;
    if (env && env->found)
        if (const char* rg = std::getenv("SC2_PORTRAIT_RIG")) {
            float m[5] = {1, 1, 1, 1, 1};
            std::sscanf(rg, "%f,%f,%f,%f,%f", &m[0], &m[1], &m[2], &m[3], &m[4]);
            scaled = *env;
            for (int k = 0; k < 3; ++k) scaled.ambient_color[k] *= m[0];
            scaled.key.color_multiplier *= m[1];
            scaled.fill.color_multiplier *= m[2];
            scaled.back.color_multiplier *= m[3];
            scaled.hdr_exposure *= m[4];
            env = &scaled;
        }
    try {
        const data::EnvLight* pe = env && env->found ? env : nullptr;
        if (std::getenv("SC2_PORTRAIT_ONE_SHOT")) {
            const render::RenderResult r = render::render_scene(scene, w, h, &cam, &store, "", false, pe);
            if (r.rgba.size() == std::size_t(w) * h * 4) return r.rgba;
            return {};
        }
        char key[160];
        std::snprintf(key, sizeof(key), "portrait:%p:%p:%s:%.4f:%zu", static_cast<const void*>(&model), static_cast<const void*>(&store),
                      pe ? pe->light_id.c_str() : "-", pe ? pe->hdr_exposure : 0.0, pe ? pe->params.size() : std::size_t(0));
        if (const int mode = portrait_load_mode(); mode == 1 || mode == 2) {
            const bool ready = render::render_scene_session_exists(key, w, h) ||
                               render::SceneRenderer::request_model_textures(model, &store, std::string(), render::render_scene_session_texture_max());
            if (mode == 2) {
                if (!ready) return {};
                static const std::size_t kBudget = std::size_t(std::getenv("SC2_PORTRAIT_UPLOAD_MB") ? std::max(0, std::atoi(std::getenv("SC2_PORTRAIT_UPLOAD_MB"))) : 0) * 1048576u;
                if (kBudget == 0 || w <= 1 || h <= 1) return std::vector<std::uint8_t>{1};
                bool up = false;
                render::render_scene_session(key, scene, w, h, cam, &store, pe, kBudget, &up);
                return up ? std::vector<std::uint8_t>{1} : std::vector<std::uint8_t>{};
            }
            if (!ready) return {};
        }
        const render::RenderResult r = render::render_scene_session(key, scene, w, h, cam, &store, pe);
        if (r.rgba.size() == std::size_t(w) * h * 4) return r.rgba;
    } catch (...) {
    }
    return {};
}

ConsoleTexture render_console_models(casc::Storage& store, const data::Catalog& cat,
                                     const std::string& skin_id, std::uint32_t width,
                                     const ConsoleView& view) {
    ConsoleTexture tex;
    if (!width) return tex;
    const data::Catalog::ConsoleSkinModels m = cat.console_skin_models(skin_id);
    if (!m.ok()) return tex;

    const std::string* paths[3] = {&m.minimap, &m.info, &m.command};
    const float* pos[3] = {m.minimap_pos, m.info_pos, m.command_pos};
    const float* scl[3] = {m.minimap_scale, m.info_scale, m.command_scale};

    std::vector<formats::Model> models(3);
    std::vector<render::SceneInstance> scene;
    float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
    for (int i = 0; i < 3; ++i) {
        bool got = false;
        for (const char* const* rp = console_asset_roots(); *rp; ++rp) {
                const char* r = *rp;
            try {
                models[std::size_t(i)] =
                    formats::M3::parse(store.read_by_name(std::string(r) + *paths[i])).decode_model();
                got = models[std::size_t(i)].vertex_count != 0;
            } catch (...) {   }
            if (got) break;
        }
        if (!got) return tex;
        render::SceneInstance inst;
        inst.model = &models[std::size_t(i)];
        inst.world = render::translation(pos[i][0], pos[i][1], pos[i][2]) *
                     render::scaling(scl[i][0], scl[i][1], scl[i][2]);
        inst.team = {0, 0, 0, -1.0f};
        inst.seq = -1;
        for (std::size_t q = 0; q < models[std::size_t(i)].sequences.size(); ++q)
            if (models[std::size_t(i)].sequences[q].name == "Stand") { inst.seq = int(q); break; }
        scene.push_back(inst);
        for (int k = 0; k < 3; ++k) {
            const float a = models[std::size_t(i)].bounds_min[k] * scl[i][k] + pos[i][k];
            const float b = models[std::size_t(i)].bounds_max[k] * scl[i][k] + pos[i][k];
            lo[k] = std::min(lo[k], std::min(a, b));
            hi[k] = std::max(hi[k], std::max(a, b));
        }
    }

    if (view.frame_w >= 1.0f && view.frame_h >= 1.0f && !std::getenv("SC2_CONSOLE_STUBS")) {
        const float kViewUnitsH = 1.5f;
        const float aspect = view.frame_w / view.frame_h;
        const float half_w = 0.5f * kViewUnitsH * aspect;
        float band = 0.0f;
        bool posed_ok = true;
        std::vector<render::SceneInstance> one_view;
        const int order[3] = {1, 0, 2};
        for (int oi = 0; oi < 3; ++oi) {
            const int i = order[oi];
            render::SceneInstance inst = scene[std::size_t(i)];
            const float ax = pos[i][0] * half_w;
            const float az = 0.5f * kViewUnitsH * (pos[i][1] + 1.0f);
            inst.world = render::translation(ax, i == 1 ? 0.5f : 0.0f, az) * render::scaling(scl[i][0], scl[i][1], scl[i][2]);
            float mn[3], mx[3];
            if (!render::posed_bounds(models[std::size_t(i)], inst.seq, 0.0f, mn, mx)) { posed_ok = false; break; }
            band = std::max(band, az + std::max(mn[2] * scl[i][2], mx[2] * scl[i][2]));
            one_view.push_back(inst);
        }
        const std::uint32_t out_w = std::uint32_t(view.frame_w + 0.5f);
        const std::uint32_t out_h = std::uint32_t(std::ceil(double(band) / double(kViewUnitsH) * double(view.frame_h)));
        if (posed_ok && band > 1e-3f && out_w && out_h) {
            const float band_units = float(out_h) * kViewUnitsH / view.frame_h;
            render::Camera cam;
            cam.orthographic = true;
            cam.ortho_half_w = half_w;
            cam.ortho_half_h = 0.5f * band_units;
            cam.transparent_clear = true;
            cam.eye = {0.0f, -5.0f, 0.5f * band_units};
            cam.center = {0.0f, 0.0f, 0.5f * band_units};
            cam.up = {0.0f, 0.0f, 1.0f};
            cam.znear = 1.0f;
            cam.zfar = 1000.0f;
            data::EnvLight env;
            {
                std::string light_id = "Console";
                if (auto l = cat.scalar("CConsoleSkin", skin_id, "Light"); l && !l->empty()) light_id = *l;
                if (!std::getenv("SC2_CONSOLE_NO_LIGHT")) env = data::resolve_environment_light(cat, light_id);
                env.params["ExposureAsAuthored"] = 1.0;
                env.params["NoShadows"] = 1.0;
                env.params["SpecNoEnergyDim"] = 1.0;
                if (view.map_env && view.map_env->found && !std::getenv("SC2_CONSOLE_NO_MAP_COLORIZE"))
                    for (const auto& kv : view.map_env->params)
                        if (kv.first == "Colorize" || kv.first.compare(0, 12, "Colorization") == 0) env.params[kv.first] = kv.second;
                if (view.map_env && view.map_env->found && view.map_env->has_hdr && view.map_env->hdr_exposure > 0.0 &&
                    !std::getenv("SC2_CONSOLE_OWN_EXPOSURE")) {
                    env.hdr_exposure = view.map_env->hdr_exposure;
                    env.has_hdr = true;
                    env.params["HDRExposure"] = view.map_env->hdr_exposure;
                }
                if (const char* fl = std::getenv("SC2_CONSOLE_LIGHT_FLIP"))
                    for (const char* c = fl; *c; ++c) {
                        const int a = *c == 'x' ? 0 : *c == 'y' ? 1 : *c == 'z' ? 2 : -1;
                        if (a < 0) continue;
                        env.key_dir[a] = -env.key_dir[a];
                        env.key.direction[a] = -env.key.direction[a];
                        env.fill.direction[a] = -env.fill.direction[a];
                        env.back.direction[a] = -env.back.direction[a];
                    }
            }
            try {
                if (render::mem_lap_marks()) render::mem_mark("console: three models decoded");
                render::RenderResult r = render::render_scene(one_view, out_w, out_h, &cam, &store, "", false, env.found ? &env : nullptr);
                if (render::mem_lap_marks()) render::mem_mark("console: renderer released");
                if (r.rgba.size() == std::size_t(out_w) * out_h * 4) {
                    for (std::size_t q = 0; q + 3 < r.rgba.size(); q += 4)
                        if (r.rgba[q + 3] >= 250 && r.rgba[q] <= 2 && r.rgba[q + 1] <= 2 && r.rgba[q + 2] <= 2) {
                            r.rgba[q] = 15;
                            r.rgba[q + 1] = 16;
                            r.rgba[q + 2] = 24;
                        }
                    tex.w = out_w;
                    tex.h = out_h;
                    tex.rgba = std::move(r.rgba);
                    return tex;
                }
            } catch (...) {
            }
        }
    }

    {
        const std::uint32_t canvas_w = std::uint32_t(app::kRefHeight * 16.0f / 9.0f + 0.5f);
        const ConsoleStubs stubs = console_stubs(store, canvas_w, std::uint32_t(app::kRefHeight));
        if (stubs.ok) {
            const float top = std::min({stubs.minimap.y, stubs.info.y, stubs.command.y});
            const float bottom = std::max({stubs.minimap.y + stubs.minimap.h,
                                           stubs.info.y + stubs.info.h,
                                           stubs.command.y + stubs.command.h});
            const int canvas_h = int(bottom - top + 0.5f);
            if (canvas_h > 0) {
                std::vector<std::uint8_t> canvas(std::size_t(canvas_w) * std::size_t(canvas_h) * 4, 0);
                const ui::Rect* rect[3] = {&stubs.minimap, &stubs.info, &stubs.command};
                ui::Rect draw_rect[3] = {stubs.minimap, stubs.info, stubs.command};
                {
                    float px_per_unit = 0.0f;
                    int agree = 0;
                    for (int i : {0, 2}) {
                        float mn[3], mx[3];
                        if (!render::posed_bounds(models[std::size_t(i)], scene[std::size_t(i)].seq, 0.0f, mn, mx)) continue;
                        const float span = std::fabs(mx[0] * scl[i][0] - mn[0] * scl[i][0]);
                        if (span > 1e-4f && rect[i]->w > 0.0f) { px_per_unit += rect[i]->w / span; ++agree; }
                    }
                    if (agree > 0) {
                        px_per_unit /= float(agree);
                        float mn[3], mx[3];
                        if (render::posed_bounds(models[1], scene[1].seq, 0.0f, mn, mx)) {
                            const float span = std::fabs(mx[0] * scl[1][0] - mn[0] * scl[1][0]);
                            const float want = span * px_per_unit;
                            if (span > 1e-4f && want > 1.0f && want < draw_rect[1].w) {
                                draw_rect[1].x += 0.5f * (draw_rect[1].w - want);
                                draw_rect[1].w = want;
                            }
                        }
                    }
                }
                bool all_drawn = true;
                const int order[3] = {1, 0, 2};
                for (int oi = 0; oi < 3 && all_drawn; ++oi) {
                    const int i = order[oi];
                    const formats::Model& mm = models[std::size_t(i)];
                    render::SceneInstance one = scene[std::size_t(i)];
                    one.world = render::scaling(scl[i][0], scl[i][1], scl[i][2]);
                    float mn[3], mx[3];
                    if (!render::posed_bounds(mm, one.seq, 0.0f, mn, mx) || !(rect[i]->w > 0.0f) || !(rect[i]->h > 0.0f)) {
                        all_drawn = false;
                        break;
                    }
                    const float x0 = std::min(mn[0] * scl[i][0], mx[0] * scl[i][0]), x1 = std::max(mn[0] * scl[i][0], mx[0] * scl[i][0]);
                    const float z0 = std::min(mn[2] * scl[i][2], mx[2] * scl[i][2]), z1 = std::max(mn[2] * scl[i][2], mx[2] * scl[i][2]);
                    if (!(x1 - x0 > 1e-4f) || !(z1 - z0 > 1e-4f)) {
                        all_drawn = false;
                        break;
                    }
                    std::vector<render::SceneInstance> solo{one};
                    render::Camera cam;
                    cam.orthographic = true;
                    cam.ortho_half_w = 0.5f * (x1 - x0);
                    cam.ortho_half_h = 0.5f * (z1 - z0);
                    cam.transparent_clear = true;
                    const float cx = 0.5f * (x0 + x1), cz = 0.5f * (z0 + z1);
                    cam.eye = {cx, -5.0f, cz};
                    cam.center = {cx, 0.0f, cz};
                    cam.up = {0.0f, 0.0f, 1.0f};
                    cam.znear = 1.0f;
                    cam.zfar = 1000.0f;
                    try {
                        const render::RenderResult r =
                            render::render_scene(solo, std::uint32_t(rect[i]->w + 0.5f),
                                                 std::uint32_t(rect[i]->h + 0.5f), &cam, &store, "");
                        if (r.rgba.size() != std::size_t(r.width) * r.height * 4) {
                            all_drawn = false;
                            break;
                        }
                        if (std::getenv("CONSOLE_DEBUG")) {
                            std::size_t drawn = 0;
                            for (std::size_t q = 3; q < r.rgba.size(); q += 4) drawn += r.rgba[q] > 8;
                            std::fprintf(stderr, "console panel %d: seq %d posed x[%.4f %.4f] y[%.4f %.4f] z[%.4f %.4f] view half %.4f x %.4f centre (%.4f, %.4f) -> %ux%u, %.1f %% covered, stub [%.0f,%.0f %.0fx%.0f]\n",
                                         i, one.seq, double(mn[0]), double(mx[0]), double(mn[1]), double(mx[1]), double(mn[2]), double(mx[2]), double(cam.ortho_half_w), double(cam.ortho_half_h), double(cx), double(cz),
                                         r.width, r.height, 100.0 * double(drawn) / double(std::max<std::size_t>(1, r.rgba.size() / 4)), double(rect[i]->x), double(rect[i]->y), double(rect[i]->w), double(rect[i]->h));
                        }
                        ui::Rect at = draw_rect[i];
                        at.y -= top;
                        blit_panel(canvas, int(canvas_w), canvas_h, r, at);
                    } catch (...) {
                        all_drawn = false;
                    }
                }
                if (all_drawn) {
                    tex.w = canvas_w;
                    tex.h = std::uint32_t(canvas_h);
                    tex.rgba = std::move(canvas);
                    return tex;
                }
            }
        }
    }

    const float kCamDist = 5.0f;
    const float span_x = hi[0] - lo[0], span_z = hi[2] - lo[2];
    if (!(span_x > 0.0f) || !(span_z > 0.0f)) return tex;
    const float authored_half_h =
        app::console_ortho_half_h(span_x, view.frame_w, view.frame_h);
    const bool stated = view.half_h > 0.0f || authored_half_h > 0.0f;
    const float half_h = view.half_h > 0.0f  ? view.half_h
                         : authored_half_h > 0.0f ? authored_half_h
                                                  : 0.5f * span_z;
    const float half_w_bounds = 0.5f * span_x;
    const std::uint32_t height =
        view.height ? view.height
                    : std::max(1u, std::uint32_t(float(width) * half_h / half_w_bounds + 0.5f));
    render::Camera cam;
    cam.orthographic = true;
    cam.ortho_half_h = half_h;
    const float cz = stated ? view.centre_z : 0.5f * (lo[2] + hi[2]);
    cam.eye = {0.5f * (lo[0] + hi[0]), -kCamDist, cz};
    cam.center = {0.5f * (lo[0] + hi[0]), 0.0f, cz};
    cam.up = {0.0f, 0.0f, 1.0f};
    cam.znear = 1.0f;
    cam.zfar = 1000.0f;

    try {
        const render::RenderResult r = render::render_scene(scene, width, height, &cam, &store, "");
        if (r.width != width || r.height != height ||
            r.rgba.size() != std::size_t(width) * height * 4)
            return tex;
        tex.w = r.width;
        tex.h = r.height;
        tex.rgba = r.rgba;
    } catch (...) {
        return tex;
    }
    return tex;
}

}
