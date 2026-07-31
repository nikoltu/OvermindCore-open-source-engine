#include "sc2/app/front_end.hpp"

#include <cmath>

#include "sc2/app/overlay_draw.hpp"
#include "sc2/app/spawn.hpp"
#include "sc2/casc/storage.hpp"
#include "sc2/data/catalog.hpp"
#include "sc2/data/economy.hpp"

namespace sc2::app {

using sim::Fixed;
using sim::Unit;

namespace {
Fixed Fx(float v) { return Fixed::from_ratio(int(std::lround(double(v) * 64.0)), 64); }
}

FrontEnd::FrontEnd(data::Catalog& cat, MeleeModels& models, casc::Storage& store,
                   const TestMap& map)
    : cat_(cat), models_(models), store_(store), map_(map) {
    menu_ctl_.focus = {map_.width * 0.5f, map_.height * 0.5f, 0.0f};
    menu_ctl_.distance = 48.0f;
    menu_ctl_.yaw = 1.5707963f;
    menu_ctl_.pitch = 1.0f;
    menu_ctl_.mirror_x = true;
}

std::vector<FrontEnd::Button> FrontEnd::menu_buttons(std::uint32_t view_w,
                                                     std::uint32_t view_h) const {
    const int bw = 380, bh = 54, gap = 18;
    int x0 = (int(view_w) - bw) / 2;
    int y0 = int(view_h) / 2 - bh;
    std::vector<Button> out;
    out.push_back({x0, y0, x0 + bw, y0 + bh, "1v1 Skirmish  vs AI", State::Skirmish});
    out.push_back({x0, y0 + bh + gap, x0 + bw, y0 + 2 * bh + gap, "Sandbox  Test Map",
                   State::Sandbox});
    return out;
}

void FrontEnd::start_sandbox() {
    sandbox_ = std::make_unique<SandboxGame>(cat_, models_, store_, map_);
    state_ = State::Sandbox;
}

void FrontEnd::start_skirmish() {
    Unit cc = make_unit(cat_, "CommandCenter", 0);
    cc.speed = Fixed();
    Unit scv = make_unit(cat_, "SCV", 0);
    Unit rax = make_unit(cat_, "Barracks", 0);
    rax.speed = Fixed();
    Unit mineral = make_unit(cat_, "MineralField", -1);
    mineral.player = -1;
    mineral.speed = Fixed();
    mineral.radius = Fixed::from_int(1);
    mineral.resource_left = 1500;
    Unit geyser = make_unit(cat_, "VespeneGeyser", -1);
    geyser.player = -1;
    geyser.speed = Fixed();
    geyser.radius = Fixed::from_int(1);
    geyser.resource_left = 2250;
    Unit marine = make_unit(cat_, "Marine", 0);
    data::UnitCost mcost = data::unit_cost(cat_, "Marine");

    MeleeSetup setup;
    for (int p = 0; p < 2; ++p) {
        const TestMap::Base& base = map_.players[p].main;
        MeleePlayerSetup ps;
        ps.start = {Fx(base.town.x), Fx(base.town.y)};
        ps.town_hall = cc;
        ps.worker = scv;
        ps.worker_count = kStartingWorkers;
        ps.mineral_field = mineral;
        ps.geyser_field = geyser;
        for (const TestMap::Resource& res : base.resources) {
            if (res.kind == TestMap::ResourceKind::Mineral)
                ps.mineral_positions.push_back({Fx(res.position.x), Fx(res.position.y)});
            else if (res.kind == TestMap::ResourceKind::Geyser)
                ps.geyser_positions.push_back({Fx(res.position.x), Fx(res.position.y)});
        }
        ps.producer = rax;
        ps.producer_mineral_cost = 150;
        ps.producer_gas_cost = 0;
        ps.producer_build_time = Fixed::from_int(46);
        ps.army_unit = marine;
        ps.army_mineral_cost = mcost.minerals;
        ps.army_gas_cost = mcost.gas;
        ps.army_build_time = Fixed::from_int(18);
        ps.start_minerals = 50;
        ps.army_threshold = 5;
        setup.players.push_back(ps);
    }

    MeleeGameConfig cfg;
    cfg.human_player = 0;
    skirmish_ = std::make_unique<MeleeGame>(cfg);
    skirmish_->setup(setup, &models_, &map_.ground, map_.width, map_.height, &map_.nav);
    skirmish_->set_ui_storage(&store_);
    state_ = State::Skirmish;
}

bool FrontEnd::step(const render::InputState& in, float dt_ms,
                    std::vector<render::SceneInstance>& scene, render::Camera& cam) {
    if (state_ == State::Skirmish && skirmish_)
        return skirmish_->step(in, dt_ms, scene, cam);
    if (state_ == State::Sandbox && sandbox_)
        return sandbox_->step(in, dt_ms, scene, cam);

    if (in.left_pressed && !prev_left_ && in.view_w && in.view_h) {
        for (const Button& b : menu_buttons(in.view_w, in.view_h)) {
            if (!b.hit(in.mouse_x, in.mouse_y)) continue;
            if (b.target == State::Skirmish) start_skirmish();
            else if (b.target == State::Sandbox) start_sandbox();
            break;
        }
    }
    prev_left_ = in.left_pressed;

    scene.clear();
    cam = menu_ctl_.camera();
    return !in.quit;
}

void FrontEnd::build_overlay(std::uint32_t view_w, std::uint32_t view_h,
                             std::vector<std::uint8_t>& out) {
    if (state_ == State::Skirmish && skirmish_) { skirmish_->build_overlay(view_w, view_h, out); return; }
    if (state_ == State::Sandbox && sandbox_) { sandbox_->build_overlay(view_w, view_h, out); return; }

    out.assign(std::size_t(view_w) * view_h * 4, 0);
    if (!view_w || !view_h) return;

    draw::fill(out, view_w, view_h, 0, 0, int(view_w), int(view_h), 8, 10, 16, 150);

    const std::string title = "SC2 ENGINE";
    draw::text(out, view_w, view_h, (int(view_w) - draw::text_w(title, 5)) / 2,
               int(view_h) / 2 - 150, title, 5, 120, 200, 255);
    const std::string sub = "select a mode";
    draw::text(out, view_w, view_h, (int(view_w) - draw::text_w(sub, 2)) / 2,
               int(view_h) / 2 - 108, sub, 2, 150, 165, 185);

    for (const Button& b : menu_buttons(view_w, view_h)) {
        draw::fill(out, view_w, view_h, b.x0, b.y0, b.x1, b.y1, 24, 34, 54, 235);
        draw::box(out, view_w, view_h, b.x0, b.y0, b.x1, b.y1, 90, 150, 220);
        draw::text(out, view_w, view_h, b.x0 + (b.x1 - b.x0 - draw::text_w(b.label, 3)) / 2,
                   b.y0 + (b.y1 - b.y0 - ui::kFontH * 3) / 2, b.label, 3, 200, 220, 245);
    }
    const std::string hint = "Esc  quit";
    draw::text(out, view_w, view_h, (int(view_w) - draw::text_w(hint, 2)) / 2,
               int(view_h) / 2 + 120, hint, 2, 130, 140, 155);
}

}
