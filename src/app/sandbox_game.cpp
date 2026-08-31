#include "sc2/app/sandbox_game.hpp"

#include <cmath>
#include <cstdio>
#include <exception>

#include "sc2/app/overlay_draw.hpp"
#include "sc2/app/shipping_wiring.hpp"
#include "sc2/app/spawn.hpp"
#include "sc2/casc/storage.hpp"
#include "sc2/data/catalog.hpp"
#include "sc2/formats/m3.hpp"
#include "sc2/render/actor.hpp"

namespace sc2::app {

using sim::Fixed;
using sim::Unit;

namespace {

Fixed Fx(float v) { return Fixed::from_ratio(int(std::lround(double(v) * 64.0)), 64); }

struct RaceDef {
    const char* town_hall;
    const char* worker;
    const char* producer;
    int producer_cost;
    const char* army;
    std::vector<const char*> spawn_list;
};

const RaceDef& race_def(Race r) {
    static const RaceDef kTerran{
        "CommandCenter", "SCV", "Barracks", 150, "Marine",
        {"SCV", "Marine", "Marauder", "Reaper", "Hellion"}};
    static const RaceDef kProtoss{
        "Nexus", "Probe", "Gateway", 150, "Zealot",
        {"Probe", "Zealot", "Stalker", "Sentry", "Adept"}};
    static const RaceDef kZerg{
        "Hatchery", "Drone", "SpawningPool", 200, "Zergling",
        {"Drone", "Zergling", "Roach", "Hydralisk", "Baneling"}};
    switch (r) {
        case Race::Protoss: return kProtoss;
        case Race::Zerg: return kZerg;
        default: return kTerran;
    }
}

const char* race_name(Race r) {
    switch (r) {
        case Race::Protoss: return "Protoss";
        case Race::Zerg: return "Zerg";
        default: return "Terran";
    }
}

const char* anim_name(render::AnimState s) {
    switch (s) {
        case render::AnimState::Birth: return "Birth";
        case render::AnimState::Stand: return "Stand";
        case render::AnimState::Walk: return "Walk";
        case render::AnimState::Attack: return "Attack";
        case render::AnimState::Death: return "Death";
        case render::AnimState::Gather: return "Gather";
        case render::AnimState::Cast: return "Cast";
        case render::AnimState::FlyStart: return "FlyStart";
        case render::AnimState::FlyStand: return "FlyStand";
        case render::AnimState::FlyWalk: return "FlyWalk";
        case render::AnimState::FlyEnd: return "FlyEnd";
    }
    return "?";
}

const char* order_name(sim::OrderType t) {
    switch (t) {
        case sim::OrderType::Idle: return "Idle";
        case sim::OrderType::Move: return "Move";
        case sim::OrderType::AttackMove: return "AttackMove";
        case sim::OrderType::Attack: return "Attack";
        case sim::OrderType::HoldPosition: return "Hold";
        case sim::OrderType::Gather: return "Gather";
        case sim::OrderType::Cast: return "Cast";
        case sim::OrderType::Patrol: return "Patrol";
        case sim::OrderType::Unload: return "Unload";
        case sim::OrderType::Load: return "Load";
    }
    return "?";
}

Unit make_structure(data::Catalog& cat, const char* token) {
    Unit u;
    try { u = make_unit(cat, token, 0); } catch (const std::exception&) {}
    u.speed = Fixed();
    return u;
}

}

RaceOpening race_opening(Race r) {
    const RaceDef& d = race_def(r);
    const char* rn = (r == Race::Protoss) ? "Protoss" : (r == Race::Zerg) ? "Zerg" : "Terran";
    return RaceOpening{d.town_hall, d.worker, d.army, rn};
}

SandboxGame::SandboxGame(data::Catalog& cat, MeleeModels& models, casc::Storage& store,
                         const TestMap& map)
    : cat_(cat), models_(models), store_(store), map_(map) {
    reset(Race::Terran);
}

void SandboxGame::reset(Race race) {
    const RaceDef& rd = race_def(race);
    const TestMap::Base& base = map_.players[0].main;

    for (const char* t : {rd.town_hall, rd.worker, rd.producer, rd.army})
        models_.ensure(store_, t);
    for (const char* t : rd.spawn_list) models_.ensure(store_, t);

    MeleeSetup setup;
    MeleePlayerSetup ps;
    ps.start = {Fx(base.town.x), Fx(base.town.y)};
    ps.town_hall = make_structure(cat_, rd.town_hall);

    Unit worker;
    try { worker = make_unit(cat_, rd.worker, 0); } catch (const std::exception&) {}
    ps.worker = worker;
    ps.worker_count = kStartingWorkers;

    Unit mineral;
    try { mineral = make_unit(cat_, "MineralField", -1); } catch (const std::exception&) {}
    mineral.player = -1;
    mineral.speed = Fixed();
    ps.mineral_field = mineral;

    Unit geyser;
    try { geyser = make_unit(cat_, "VespeneGeyser", -1); } catch (const std::exception&) {}
    geyser.player = -1;
    geyser.speed = Fixed();
    ps.geyser_field = geyser;

    for (const TestMap::Resource& res : base.resources) {
        if (res.kind == TestMap::ResourceKind::Mineral)
            ps.mineral_positions.push_back({Fx(res.position.x), Fx(res.position.y)});
        else if (res.kind == TestMap::ResourceKind::Geyser)
            ps.geyser_positions.push_back({Fx(res.position.x), Fx(res.position.y)});
    }

    ps.producer = make_structure(cat_, rd.producer);
    ps.producer_mineral_cost = rd.producer_cost;
    ps.producer_build_time = Fixed::from_int(30);
    try { ps.army_unit = make_unit(cat_, rd.army, 0); } catch (const std::exception&) {}
    ps.army_mineral_cost = 50;
    ps.army_build_time = Fixed::from_int(1);
    ps.start_minerals = 50000;
    ps.army_threshold = 100000;
    setup.players.push_back(ps);

    MeleeGameConfig cfg;
    cfg.human_player = 0;
    cfg.no_victory = true;
    game_ = std::make_unique<MeleeGame>(cfg);
    game_->setup(setup, &models_, &map_.ground, map_.width, map_.height, &map_.nav);
    game_->set_ui_storage(&store_);
    apply_shipping_wiring(*game_, cat_, nullptr, nullptr, nullptr);

    float sx = base.town.x, sy = base.town.y;
    float dx = map_.width * 0.5f - sx, dy = map_.height * 0.5f - sy;
    float len = std::sqrt(dx * dx + dy * dy);
    if (len < 1e-3f) len = 1.0f;
    marker_x_ = Fx(sx + dx / len * 8.0f);
    marker_y_ = Fx(sy + dy / len * 8.0f);

    race_ = race;
    spawn_seq_ = 0;
}

void SandboxGame::spawn_token(const std::string& token) {
    if (!game_) return;
    const formats::Model* m = models_.ensure(store_, token);
    Unit proto;
    try { proto = make_unit(cat_, token, 0); } catch (const std::exception&) { return; }
    const float ox = float(spawn_seq_ % 5) * 0.8f - 1.6f;
    const float oy = float(spawn_seq_ / 5 % 5) * 0.8f;
    ++spawn_seq_;
    game_->spawn(proto, token, m, marker_x_ + Fx(ox), marker_y_ + Fx(oy));
}

std::vector<SandboxGame::Button> SandboxGame::race_buttons(std::uint32_t view_w,
                                                           std::uint32_t  ) const {
    const int bw = 118, bh = 26, gap = 6, n = 3;
    const int total = n * bw + (n - 1) * gap;
    int x = (int(view_w) - total) / 2, y = 6;
    std::vector<Button> out;
    const char* labels[3] = {"Terran", "Protoss", "Zerg"};
    const char* tokens[3] = {"race:terran", "race:protoss", "race:zerg"};
    for (int i = 0; i < n; ++i) {
        out.push_back({x, y, x + bw, y + bh, labels[i], tokens[i]});
        x += bw + gap;
    }
    return out;
}

std::vector<SandboxGame::Button> SandboxGame::spawn_buttons(std::uint32_t view_w,
                                                            std::uint32_t  ) const {
    const RaceDef& rd = race_def(race_);
    const int bw = 150, bh = 26, gap = 4;
    int x0 = int(view_w) - bw - 8, y0 = 64;
    std::vector<Button> out;
    for (const char* t : rd.spawn_list) {
        out.push_back({x0, y0, x0 + bw, y0 + bh, std::string("+ ") + t, t});
        y0 += bh + gap;
    }
    return out;
}

bool SandboxGame::step(const render::InputState& in, float dt_ms,
                       std::vector<render::SceneInstance>& scene, render::Camera& cam) {
    render::InputState in2 = in;

    if (in.key_debug && !prev_f4_) debug_ = !debug_;
    prev_f4_ = in.key_debug;

    if (in.left_pressed && !prev_left_ && in.view_w && in.view_h) {
        bool consumed = false;
        for (const Button& b : race_buttons(in.view_w, in.view_h)) {
            if (!b.hit(in.mouse_x, in.mouse_y)) continue;
            Race r = b.token == "race:protoss" ? Race::Protoss
                     : b.token == "race:zerg"  ? Race::Zerg
                                               : Race::Terran;
            reset(r);
            consumed = true;
            break;
        }
        if (!consumed) {
            for (const Button& b : spawn_buttons(in.view_w, in.view_h)) {
                if (!b.hit(in.mouse_x, in.mouse_y)) continue;
                spawn_token(b.token);
                consumed = true;
                break;
            }
        }
        if (consumed) {
            in2.left_pressed = false;
            in2.left_down = false;
        }
    }
    prev_left_ = in.left_pressed;

    return game_->step(in2, dt_ms, scene, cam);
}

void SandboxGame::build_overlay(std::uint32_t view_w, std::uint32_t view_h,
                                std::vector<std::uint8_t>& out) {
    if (!game_) return;
    game_->build_overlay(view_w, view_h, out);
    if (!view_w || !view_h || out.empty()) return;

    for (const Button& b : race_buttons(view_w, view_h)) {
        bool cur = (b.token == "race:terran" && race_ == Race::Terran) ||
                   (b.token == "race:protoss" && race_ == Race::Protoss) ||
                   (b.token == "race:zerg" && race_ == Race::Zerg);
        draw::fill(out, view_w, view_h, b.x0, b.y0, b.x1, b.y1,
                  cur ? 40 : 18, cur ? 90 : 22, cur ? 40 : 28, 220);
        draw::box(out, view_w, view_h, b.x0, b.y0, b.x1, b.y1, 120, 200, 130);
        draw::text(out, view_w, view_h, b.x0 + 10, b.y0 + 6, b.label, 2,
             cur ? 200 : 170, 255, cur ? 200 : 170);
    }

    {
        std::vector<Button> sb = spawn_buttons(view_w, view_h);
        draw::text(out, view_w, view_h, int(view_w) - 158, 44, "SPAWN", 2, 235, 220, 120);
        for (const Button& b : sb) {
            draw::fill(out, view_w, view_h, b.x0, b.y0, b.x1, b.y1, 22, 30, 46, 220);
            draw::box(out, view_w, view_h, b.x0, b.y0, b.x1, b.y1, 90, 130, 190);
            draw::text(out, view_w, view_h, b.x0 + 8, b.y0 + 6, b.label, 2, 190, 210, 245);
        }
    }

    if (debug_) {
        const sim::World& w = game_->world();
        int units = 0, bldg = 0;
        for (const Unit& u : w.units()) {
            if (!u.alive || u.player != 0) continue;
            if (u.speed.raw() == 0) ++bldg; else ++units;
        }
        int y = 30;
        char line[128];
        std::snprintf(line, sizeof(line), "DEBUG  units:%d  bldg:%d  race:%s", units, bldg,
                      race_name(race_));
        draw::text(out, view_w, view_h, 6, y, line, 2, 120, 235, 255);
        y += 22;

        const std::vector<std::uint32_t>& sel = game_->selection();
        if (!sel.empty() && sel[0] < w.units().size()) {
            std::uint32_t id = sel[0];
            const Unit& u = w.units()[id];
            std::string token =
                (id < game_->layout().unit_type.size()) ? game_->layout().unit_type[id] : "?";
            const char* an = "-";
            if (id < game_->driver().actors.size())
                an = anim_name(game_->driver().actors[id].current());
            const char* act = u.orders.empty() ? "Idle" : order_name(u.orders.front().type);
            std::snprintf(line, sizeof(line), "SEL %s  anim:%s  act:%s", token.c_str(), an, act);
            draw::text(out, view_w, view_h, 6, y, line, 2, 200, 230, 160);
            y += 22;
            std::snprintf(line, sizeof(line), "spd:%.2f  r:%.2f  hp:%d/%d  pos:%.1f,%.1f",
                          u.speed.to_double(), u.radius.to_double(), int(u.hp.to_double()),
                          int(u.hp_max.to_double()), u.x.to_double(), u.y.to_double());
            draw::text(out, view_w, view_h, 6, y, line, 2, 200, 230, 160);
        } else {
            draw::text(out, view_w, view_h, 6, y, "SEL none (drag-select a unit)", 2, 150, 160, 175);
        }
    }
}

}
