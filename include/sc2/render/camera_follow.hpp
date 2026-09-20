#pragma once
#include <algorithm>
#include <cmath>

namespace sc2::render {

struct CameraFollowParams {
    float leash_l = -9.5f, leash_b = -10.0f, leash_r = 9.5f, leash_t = 10.0f;
    float band_x0 = -4.0f, band_x1 = 4.0f, band_y0 = -3.0f, band_y1 = 3.0f;
    float timeout_normal = 6.0f, timeout_leashed = 0.0625f, timeout_unleashed = 12.0f;
    float jump_distance = 17.0f, jump_delay = 1.0f;
    float decay_duration = 2.0f, decay_factor = 0.7f;
    float velocity_max = 80.0f;
};

struct CameraFollow {
    CameraFollowParams p;
    bool active = false;
    float resume_in = 0.0f;
    float cut_in = -1.0f;

    void start() { active = true; resume_in = 0.0f; cut_in = -1.0f; }
    void stop() { active = false; resume_in = 0.0f; cut_in = -1.0f; }

    bool update(float tx, float ty, float& fx, float& fy, float dt, bool user_scrolled) {
        if (!active || dt <= 0.0f) return false;
        const float dx = tx - fx, dy = ty - fy;
        const bool leashed = dx >= p.leash_l && dx <= p.leash_r && dy >= p.leash_b && dy <= p.leash_t;
        if (user_scrolled) {
            resume_in = leashed ? p.timeout_leashed : p.timeout_unleashed;
            if (resume_in < p.timeout_normal && !leashed) resume_in = p.timeout_normal;
            cut_in = -1.0f;
            return false;
        }
        if (resume_in > 0.0f) {
            resume_in -= dt;
            if (resume_in > 0.0f) return false;
            resume_in = 0.0f;
        }
        const float dist = std::sqrt(dx * dx + dy * dy);
        if (dist > p.jump_distance) {
            if (cut_in < 0.0f) cut_in = p.jump_delay;
            cut_in -= dt;
            if (cut_in > 0.0f) return false;
            fx = tx;
            fy = ty;
            cut_in = -1.0f;
            return true;
        }
        cut_in = -1.0f;
        if (dx >= p.band_x0 && dx <= p.band_x1 && dy >= p.band_y0 && dy <= p.band_y1) return false;
        const float keep = p.decay_duration > 0.0f ? std::pow(p.decay_factor, dt / p.decay_duration) : 0.0f;
        float mx = dx * (1.0f - keep), my = dy * (1.0f - keep);
        const float step = std::sqrt(mx * mx + my * my), cap = p.velocity_max * dt;
        if (step > cap && step > 0.0f) {
            mx *= cap / step;
            my *= cap / step;
        }
        fx += mx;
        fy += my;
        return true;
    }
};

}
