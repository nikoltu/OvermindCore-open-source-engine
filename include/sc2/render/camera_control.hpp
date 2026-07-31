#pragma once

#include <algorithm>
#include <cmath>
#include <vector>

#include "sc2/render/mat4.hpp"
#include "sc2/render/scene.hpp"

namespace sc2::render {

struct CameraControl {
    Vec3 focus{0, 0, 0};
    float distance = 20.0f;
    float yaw = 0.7853981634f;
    float pitch = 1.0f;

    float min_distance = 2.0f;
    float max_distance = 200.0f;
    float min_pitch = 0.15f;
    float max_pitch = 1.55f;

    float fovy = 0.7854f;
    float znear = 0.1f;
    float zfar = 1000.0f;
    Vec3 world_up{0, 0, 1};
    bool mirror_x = false;

    Vec3 offset_dir() const {
        float cp = std::cos(pitch), sp = std::sin(pitch);
        return {cp * std::cos(yaw), cp * std::sin(yaw), sp};
    }
    Vec3 eye() const {
        Vec3 d = offset_dir();
        return {focus.x + d.x * distance, focus.y + d.y * distance,
                focus.z + d.z * distance};
    }
    Vec3 forward() const {
        Vec3 d = offset_dir();
        return {-d.x, -d.y, -d.z};
    }
    Vec3 right() const { return normalize(cross(forward(), world_up)); }
    Vec3 cam_up() const { return cross(right(), forward()); }

    Vec3 ground_right() const {
        Vec3 r = right();
        Vec3 g = normalize(Vec3{r.x, r.y, 0.0f});
        if (g.x == 0 && g.y == 0 && g.z == 0) return {1, 0, 0};
        return g;
    }
    Vec3 ground_forward() const {
        Vec3 f = forward();
        Vec3 g = normalize(Vec3{f.x, f.y, 0.0f});
        if (g.x == 0 && g.y == 0 && g.z == 0) return {0, 1, 0};
        return g;
    }

    void pan(float right_amt, float forward_amt) {
        Vec3 gr = ground_right(), gf = ground_forward();
        const float rx = mirror_x ? -right_amt : right_amt;
        focus.x += gr.x * rx + gf.x * forward_amt;
        focus.y += gr.y * rx + gf.y * forward_amt;
    }
    void pan_world(float dx, float dy) {
        focus.x += dx;
        focus.y += dy;
    }

    void zoom(float delta) {
        distance = std::clamp(distance + delta, min_distance, max_distance);
    }

    void orbit(float dyaw, float dpitch = 0.0f) {
        yaw += dyaw;
        pitch = std::clamp(pitch + dpitch, min_pitch, max_pitch);
    }

    Camera camera() const {
        Camera c;
        c.eye = eye();
        c.center = focus;
        c.up = world_up;
        c.fovy = fovy;
        c.znear = znear;
        c.zfar = zfar;
        c.mirror_x = mirror_x;
        return c;
    }
    Mat4 view_proj(float aspect) const { return camera().view_proj(aspect); }

    void fit(const std::vector<Vec3>& points, float aspect, float margin = 1.05f) {
        if (points.empty() || aspect <= 0.0f) return;

        Vec3 lo = points[0], hi = points[0];
        for (const Vec3& p : points) {
            lo.x = std::min(lo.x, p.x); lo.y = std::min(lo.y, p.y); lo.z = std::min(lo.z, p.z);
            hi.x = std::max(hi.x, p.x); hi.y = std::max(hi.y, p.y); hi.z = std::max(hi.z, p.z);
        }
        focus = {(lo.x + hi.x) * 0.5f, (lo.y + hi.y) * 0.5f, (lo.z + hi.z) * 0.5f};

        const Vec3 f = forward(), s = right(), u = cam_up();
        const float t = std::tan(fovy * 0.5f);
        const float tx = aspect * t;

        float need = min_distance;
        for (const Vec3& p : points) {
            Vec3 q = sub(p, focus);
            float fq = dot(f, q);
            float dx_need = std::fabs(dot(s, q)) / tx - fq;
            float dy_need = std::fabs(dot(u, q)) / t - fq;
            need = std::max(need, std::max(dx_need, dy_need));
        }
        distance = std::max(need * margin, min_distance);

        float dmin = 1e30f, dmax = -1e30f;
        for (const Vec3& p : points) {
            float D = dot(f, sub(p, focus)) + distance;
            dmin = std::min(dmin, D);
            dmax = std::max(dmax, D);
        }
        znear = std::max(0.01f, dmin * 0.5f);
        zfar = std::max(dmax * 2.0f + 1.0f, znear + 1.0f);
    }
};

}
