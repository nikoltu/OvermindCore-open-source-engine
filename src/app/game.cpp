#include "sc2/app/game.hpp"

#include <cmath>

#include "sc2/app/control.hpp"
#include "sc2/formats/m3.hpp"
#include "sc2/render/mat4.hpp"

namespace sc2::app {

using sim::Fixed;
using sim::Order;
using sim::OrderType;
using sim::Unit;

namespace {

void set_stats(Unit& u, float hp, float dmg, float period, float range, float speed,
               float radius) {
    u.hp_max = u.hp = Fixed::from_ratio(int(hp * 64), 64);
    u.wpn_damage = Fixed::from_ratio(int(dmg * 64), 64);
    u.wpn_period = Fixed::from_ratio(int(period * 64), 64);
    u.wpn_range = Fixed::from_ratio(int(range * 64), 64);
    u.speed = Fixed::from_ratio(int(speed * 64), 64);
    u.radius = Fixed::from_ratio(int(radius * 64), 64);
}

std::array<float, 4> team_colour(int player, int human, bool selected) {
    std::array<float, 4> c = (player == human)
                                 ? std::array<float, 4>{0.85f, 0.15f, 0.12f, 1.0f}
                                 : std::array<float, 4>{0.15f, 0.35f, 0.90f, 1.0f};
    if (selected)
        for (int i = 0; i < 3; ++i) c[i] = std::min(1.0f, c[i] + 0.35f);
    return c;
}

}

DemoLayout build_demo(sim::World& w, int human_player, int ai_player, int n_marines,
                      int n_zerglings) {
    DemoLayout L;
    for (int i = 0; i < n_marines; ++i) {
        Unit u;
        u.player = human_player;
        u.x = Fixed::from_int(3 + i * 2);
        u.y = Fixed::from_int(3);
        set_stats(u, 45, 6, 0.8608f, 5.0f, 2.25f, 0.375f);
        L.marines.push_back(w.add_unit(u));
    }
    for (int i = 0; i < n_zerglings; ++i) {
        Unit u;
        u.player = ai_player;
        u.x = Fixed::from_int(2 + i * 2);
        u.y = Fixed::from_int(15);
        set_stats(u, 35, 5, 0.696f, 0.1f, 2.953125f, 0.375f);
        L.zerglings.push_back(w.add_unit(u));
    }
    return L;
}

Skirmish::Skirmish(SkirmishConfig cfg) : cfg_(cfg), world_(1) {}

void Skirmish::setup_demo() {
    layout_ = build_demo(world_, cfg_.human_player, cfg_.ai_player, cfg_.n_marines,
                         cfg_.n_zerglings);

    float ms = 1.0f;
    if (cfg_.marine && cfg_.marine->vertex_count &&
        cfg_.marine->positions.size() >= size_t(cfg_.marine->vertex_count) * 3) {
        float mn = 1e30f, mx = -1e30f;
        for (uint32_t i = 0; i < cfg_.marine->vertex_count; ++i)
            for (int a = 0; a < 2; ++a) {
                float c = cfg_.marine->positions[i * 3 + a];
                mn = c < mn ? c : mn;
                mx = c > mx ? c : mx;
            }
        float diam = mx - mn;
        if (diam > 1e-3f) ms = cfg_.model_size / diam;
    }
    layout_.model_scale = ms;

    driver_.default_model = cfg_.marine;
    for (uint32_t id : layout_.marines) driver_.set_model(id, cfg_.marine);
    for (uint32_t id : layout_.zerglings) driver_.set_model(id, cfg_.zergling);

    render::Vec3 c{0, 0, 0};
    int n = 0;
    for (const Unit& u : world_.units()) {
        c.x += float(u.x.to_double());
        c.y += float(u.y.to_double());
        ++n;
    }
    if (n) {
        camctl_.focus = {c.x / float(n), c.y / float(n), 0};
        camctl_.distance = 20.0f;
    }

    if (cfg_.enable_ai) {
        ai::MeleeConfig aic;
        aic.army_threshold = 1;
        aic.rally_x = Fixed::from_int(8);
        aic.rally_y = Fixed::from_int(15);
        aic.enemy_x = Fixed::from_int(8);
        aic.enemy_y = Fixed::from_int(3);
        ai_ = std::make_unique<ai::MeleeAI>(cfg_.ai_player, aic);
        ai_->set_base(ai::MeleeBase{});
    }
}

void Skirmish::update_camera(const render::InputState& in, float dt_s) {
    if (in.wheel != 0.0f) camctl_.zoom(-in.wheel * cfg_.zoom_step);

    float px = 0, py = 0;
    if (in.key_pan_left) px -= 1;
    if (in.key_pan_right) px += 1;
    if (in.key_pan_down) py -= 1;
    if (in.key_pan_up) py += 1;
    if (in.view_w && in.view_h) {
        if (in.mouse_x <= cfg_.edge_margin) px -= 1;
        else if (in.mouse_x >= float(in.view_w) - cfg_.edge_margin) px += 1;
        if (in.mouse_y <= cfg_.edge_margin) py += 1;
        else if (in.mouse_y >= float(in.view_h) - cfg_.edge_margin) py -= 1;
    }
    if (px != 0.0f || py != 0.0f)
        camctl_.pan(px * cfg_.pan_speed * dt_s, py * cfg_.pan_speed * dt_s);

    float o = 0;
    if (in.key_orbit_left) o += 1;
    if (in.key_orbit_right) o -= 1;
    if (o != 0.0f) camctl_.orbit(o * cfg_.orbit_speed * dt_s);
    if (in.middle_down && (in.mouse_dx != 0.0f || in.mouse_dy != 0.0f))
        camctl_.orbit(-in.mouse_dx * cfg_.orbit_mouse, -in.mouse_dy * cfg_.orbit_mouse);
}

void Skirmish::handle_commands(const render::InputState& in, float aspect) {
    if (!in.view_w || !in.view_h) return;
    render::Mat4 mvp = camctl_.view_proj(aspect);
    Control ctrl(mvp, in.view_w, in.view_h, cfg_.human_player);

    if (in.left_pressed) {
        dragging_ = true;
        drag_x0_ = in.mouse_x;
        drag_y0_ = in.mouse_y;
    }
    if (in.left_released && dragging_) {
        dragging_ = false;
        float x0 = drag_x0_, y0 = drag_y0_, x1 = in.mouse_x, y1 = in.mouse_y;
        if (std::fabs(x1 - x0) < 6.0f && std::fabs(y1 - y0) < 6.0f) {
            x0 = in.mouse_x - 6.0f; x1 = in.mouse_x + 6.0f;
            y0 = in.mouse_y - 6.0f; y1 = in.mouse_y + 6.0f;
        }
        selection_ = ctrl.box_select(world_, ScreenRect{x0, y0, x1, y1});
    }
    if (in.right_pressed && !selection_.empty()) {
        ctrl.issue_command(world_, selection_, ScreenPoint{in.mouse_x, in.mouse_y},
                           in.shift);
    }
}

void Skirmish::emit_scene(std::vector<render::SceneInstance>& scene) {
    scene.clear();
    if (cfg_.ground) {
        render::SceneInstance g;
        g.model = cfg_.ground;
        g.world = render::Mat4::identity();
        g.team = {0, 0, 0, -1.0f};
        g.seq = -1;
        scene.push_back(g);
    }

    const float ms = layout_.model_scale;
    for (const Unit& u : world_.units()) {
        if (!u.alive) continue;
        render::SceneInstance inst;
        const formats::Model* m =
            (u.id < driver_.actors.size()) ? driver_.actors[u.id].model : nullptr;
        if (!m) m = (u.player == cfg_.human_player) ? cfg_.marine : cfg_.zergling;
        inst.model = m;
        inst.world = render::translation(float(u.x.to_double()), float(u.y.to_double()), 0.0f) *
                     render::scaling(ms, ms, ms);
        if (u.id < driver_.actors.size() && m) {
            render::ActorInstance& a = driver_.actors[u.id];
            inst.seq = a.sequence_of(a.current());
            inst.time_ms = a.clock;
        }
        bool selected = false;
        for (uint32_t s : selection_)
            if (s == u.id) { selected = true; break; }
        inst.team = team_colour(u.player, cfg_.human_player, selected);
        scene.push_back(inst);
    }
}

bool Skirmish::step(const render::InputState& in, float dt_ms,
                    std::vector<render::SceneInstance>& scene, render::Camera& cam) {
    const float dt_s = dt_ms / 1000.0f;
    const float aspect =
        in.view_h ? float(in.view_w) / float(in.view_h) : 1.0f;

    update_camera(in, dt_s);

    if (!framed_ && in.view_w && in.view_h) {
        std::vector<render::Vec3> pts;
        for (const Unit& u : world_.units())
            if (u.alive)
                pts.push_back({float(u.x.to_double()), float(u.y.to_double()), 0.0f});
        camctl_.fit(pts, aspect);
        framed_ = true;
    }

    handle_commands(in, aspect);

    const float tick_ms = 1000.0f / float(sim::World::kTicksPerSecond);
    accum_ms_ += dt_ms;
    if (accum_ms_ > tick_ms * 8.0f) accum_ms_ = tick_ms * 8.0f;
    while (accum_ms_ >= tick_ms) {
        world_.step();
        accum_ms_ -= tick_ms;
        if (ai_ && ++tick_counter_ >= cfg_.ai_period_ticks) {
            ai_->update(world_);
            tick_counter_ = 0;
        }
    }

    driver_.observe(world_, dt_ms);
    emit_scene(scene);
    cam = camctl_.camera();
    return !in.quit;
}

}
