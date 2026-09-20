#include "sc2/render/offscreen.hpp"

#include "sc2/render/anim.hpp"
#include "sc2/render/scene.hpp"

#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <algorithm>
#include <vector>

#include "sc2/app/console_panel.hpp"
#include "sc2/data/catalog.hpp"
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
                                          int cam_bone, float znear, float zfar, std::uint32_t w, std::uint32_t h) {
    if (!w || !h || model.vertex_count == 0) return {};
    render::SceneInstance inst;
    inst.model = &model;
    inst.world = render::Mat4::identity();
    inst.team = {0, 0, 0, -1.0f};
    inst.seq = model.sequences.empty() ? -1 : 0;
    for (std::size_t q = 0; q < model.sequences.size(); ++q)
        if (model.sequences[q].name == "Stand") { inst.seq = int(q); break; }
    inst.time_ms = 0.0f;
    const float fov = fov_rad > 0.01f && fov_rad < 3.0f ? fov_rad : 0.654498f;
    render::Camera cam;
    cam.fovy = fov;
    cam.znear = znear > 0.0f ? znear : 0.01f;
    cam.zfar = zfar > cam.znear ? zfar : 100.0f;
    bool posed = false;
    if (cam_bone >= 0 && std::size_t(cam_bone) < model.bones.size()) {
        const std::vector<render::Mat4> bw = render::pose_world(model, inst.seq >= 0 ? std::uint32_t(inst.seq) : 0u, 0.0f);
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
    std::vector<render::SceneInstance> scene{inst};
    try {
        const render::RenderResult r = render::render_scene(scene, w, h, &cam, &store, "");
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
                        ui::Rect at = *rect[i];
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
