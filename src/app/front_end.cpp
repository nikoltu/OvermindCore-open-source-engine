#include "sc2/app/front_end.hpp"

#include <cmath>
#include <fstream>
#include <iterator>

#include "sc2/app/ability.hpp"
#include "sc2/app/overlay_draw.hpp"
#include "sc2/app/race_melee_setup.hpp"
#include "sc2/app/spawn.hpp"
#include "sc2/casc/storage.hpp"
#include "sc2/data/catalog.hpp"
#include "sc2/data/economy.hpp"
#include "sc2/data/production.hpp"

namespace sc2::app {

using sim::Fixed;
using sim::Unit;

namespace {
Fixed Fx(float v) { return Fixed::from_ratio(int(std::lround(double(v) * 64.0)), 64); }

const char* race_name(Race r) {
    switch (r) {
        case Race::Protoss: return "Protoss";
        case Race::Zerg:    return "Zerg";
        case Race::Terran:
        default:            return "Terran";
    }
}
Race race_cycle(Race r) {
    switch (r) {
        case Race::Terran:  return Race::Protoss;
        case Race::Protoss: return Race::Zerg;
        case Race::Zerg:
        default:            return Race::Terran;
    }
}

constexpr int kActStart = 1, kActBack = 2, kActMapsFolder = 3, kActP0Race = 10, kActP1Race = 11,
              kMapRow0 = 100;
}

FrontEnd::FrontEnd(data::Catalog& cat, MeleeModels& models, casc::Storage& store,
                   const TestMap& map)
    : cat_(cat), models_(models), store_(store), map_(map) {
    menu_ctl_.focus = {map_.width * 0.5f, map_.height * 0.5f, 0.0f};
    menu_ctl_.distance = 48.0f;
    menu_ctl_.yaw = 1.5707963f;
    menu_ctl_.pitch = 1.0f;
    menu_ctl_.mirror_x = true;
    maps_.push_back({"", "Synthetic Arena (72x72)"});
}

void FrontEnd::set_maps(std::vector<platform::MapEntry> maps) {
    maps_.assign(1, platform::MapEntry{"", "Synthetic Arena (72x72)"});
    for (auto& m : maps) maps_.push_back(std::move(m));
    sel_map_ = 0;
}

std::vector<FrontEnd::Button> FrontEnd::menu_buttons(std::uint32_t view_w,
                                                     std::uint32_t view_h) const {
    const int bw = 380, bh = 54, gap = 18;
    int x0 = (int(view_w) - bw) / 2;
    int y0 = int(view_h) / 2 - bh;
    std::vector<Button> out;
    out.push_back({x0, y0, x0 + bw, y0 + bh, "1v1 Melee  vs AI", 1});
    out.push_back({x0, y0 + bh + gap, x0 + bw, y0 + 2 * bh + gap, "Sandbox  Test Map", 2});
    return out;
}

std::vector<FrontEnd::Button> FrontEnd::lobby_buttons(std::uint32_t view_w,
                                                      std::uint32_t view_h) const {
    std::vector<Button> out;
    const int W = int(view_w), H = int(view_h);
    const int lx0 = 48, lx1 = W / 2 - 24, rowh = 30, gap = 4, listy = 132;
    const int maxrows = std::max(1, (H - listy - 140) / (rowh + gap));
    for (int i = 0; i < int(maps_.size()) && i < maxrows; ++i) {
        int y = listy + i * (rowh + gap);
        out.push_back({lx0, y, lx1, y + rowh, maps_[i].name, kMapRow0 + i});
    }
    const int rx0 = W / 2 + 24, rx1 = W - 48, rbh = 48;
    out.push_back({rx0, listy + 24, rx1, listy + 24 + rbh,
                   std::string("Your Race:  ") + race_name(p0_race_), kActP0Race});
    out.push_back({rx0, listy + 24 + rbh + 20, rx1, listy + 24 + 2 * rbh + 20,
                   std::string("Opponent:  ") + race_name(p1_race_), kActP1Race});
    if (maps_folder_cb_)
        out.push_back({rx0, listy + 24 + 2 * rbh + 60, rx1, listy + 24 + 3 * rbh + 60,
                       "Maps Folder...", kActMapsFolder});
    const int bw = 200, bh = 52;
    int by = H - 96;
    out.push_back({W / 2 + 12, by, W / 2 + 12 + bw, by + bh, "START", kActStart});
    out.push_back({W / 2 - 12 - bw, by, W / 2 - 12, by + bh, "Back", kActBack});
    return out;
}

void FrontEnd::start_sandbox() {
    sandbox_ = std::make_unique<SandboxGame>(cat_, models_, store_, map_);
    state_ = State::Sandbox;
}

void FrontEnd::start_skirmish() {
    p0_race_ = Race::Terran;
    p1_race_ = Race::Terran;
    sel_map_ = 0;
    start_lobby_match();
}

void FrontEnd::start_lobby_match() {
    MeleeSetup setup;
    const formats::Model* ground = nullptr;
    float map_w = 0, map_h = 0;
    const sim::NavGrid* nav = nullptr;

    if (sel_map_ <= 0 || maps_[sel_map_].path.empty()) {
        data::Production prod;
        prod.build(cat_);
        const Race rs[2] = {p0_race_, p1_race_};
        for (int p = 0; p < 2; ++p) {
            const MeleeProtos pr = race_protos(cat_, rs[p]);
            const TestMap::Base& base = map_.players[p].main;
            MeleePlayerSetup ps;
            ps.start = {Fx(base.town.x), Fx(base.town.y)};
            ps.town_hall = pr.hall;
            ps.worker = pr.worker;
            ps.worker_count = kStartingWorkers;
            ps.mineral_field = pr.mineral;
            ps.geyser_field = pr.geyser;
            for (const TestMap::Resource& res : base.resources) {
                if (res.kind == TestMap::ResourceKind::Mineral)
                    ps.mineral_positions.push_back({Fx(res.position.x), Fx(res.position.y)});
                else if (res.kind == TestMap::ResourceKind::Geyser)
                    ps.geyser_positions.push_back({Fx(res.position.x), Fx(res.position.y)});
            }
            ps.producer = pr.producer;
            ps.supply_unit = pr.supply;
            ps.army_unit = pr.army;
            ps.start_minerals = 50;
            apply_race_costs(ps, cat_, rs[p], prod);
            setup.players.push_back(ps);
        }
        ground = &map_.ground;
        map_w = map_.width;
        map_h = map_.height;
        nav = &map_.nav;
    } else {
        std::ifstream in(maps_[sel_map_].path, std::ios::binary);
        std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(in)),
                                        std::istreambuf_iterator<char>());
        real_map_ = std::make_unique<formats::LoadedMap>(
            formats::load_map_terrain(std::move(bytes)));
        real_ground_ = formats::build_map_mesh(*real_map_);
        if (texture_cb_) texture_cb_(real_ground_, *real_map_);
        setup = melee_setup_for_map(*real_map_, cat_, p0_race_, p1_race_);
        ground = &real_ground_;
        map_w = float(real_map_->width);
        map_h = float(real_map_->height);
        nav = &real_map_->nav;
    }

    const Race rs2[2] = {p0_race_, p1_race_};
    for (int p = 0; p < 2; ++p) {
        const RaceUnits u = race_units(rs2[p]);
        for (const char* tok : {u.hall, u.worker, u.producer, u.supply, u.army})
            models_.ensure(store_, tok);
    }

    MeleeGameConfig cfg;
    cfg.human_player = 0;
    skirmish_ = std::make_unique<MeleeGame>(cfg);
    skirmish_->set_ui_storage(&store_);
    skirmish_->set_build_verb(p0_race_ == Race::Protoss ? sim::BuildVerb::Warp
                              : p0_race_ == Race::Zerg  ? sim::BuildVerb::Morph
                                                        : sim::BuildVerb::Committed);
    skirmish_->register_creep_masks(cat_);
    skirmish_->setup(setup, &models_, ground, map_w, map_h, nav);
    {
        data::Production tech_prod;
        tech_prod.build(cat_);
        for (const std::string& uid : cat_.ids("CUnit"))
            for (const data::ProductInfo& pi : tech_prod.producers_of(uid))
                if (pi.requirement.find("Attached") != std::string::npos &&
                    pi.requirement.find("TechLab") != std::string::npos) {
                    skirmish_->world().require_attached_techlab(uid);
                    break;
                }
    }
    skirmish_->enable_larva(cat_);
    skirmish_->enable_collapsible_debris(cat_);
    skirmish_->enable_ai_casting_from_roster(cat_);
    if (!research_up_) {
        research_up_ = std::make_unique<data::UpgradeCatalog>();
        try {
            research_up_->load_melee(store_);
        } catch (const std::exception&) {
        }
    }
    {
        const Race rs_res[2] = {p0_race_, p1_race_};
        auto race_tech_building = [](Race r) -> std::string {
            switch (r) {
                case Race::Protoss: return "Forge";
                case Race::Zerg:    return "EvolutionChamber";
                case Race::Terran:
                default:            return "EngineeringBay";
            }
        };
        std::vector<MeleeResearchDriver::ProtoNames> names(setup.players.size());
        std::vector<MeleeGame::AiResearchPlan> plans(setup.players.size());
        for (std::size_t p = 0; p < setup.players.size(); ++p) {
            const MeleePlayerSetup& ps = setup.players[p];
            names[p] = MeleeResearchDriver::ProtoNames{ps.army_unit.catalog_id, ps.worker.catalog_id,
                                                       ps.producer.catalog_id, ps.supply_unit.catalog_id};
            plans[p].upgrades =
                army_weapon_upgrade_ids(cat_, *research_up_, ps.army_unit.catalog_id);
            plans[p].tech_id = race_tech_building(p < 2 ? rs_res[p] : Race::Terran);
        }
        skirmish_->enable_research(cat_, *research_up_, std::move(names));
        skirmish_->enable_ai_research(std::move(plans));
    }
    skirmish_->world().set_weapon_executor(true);
    skirmish_->world().set_reserve_supply(true);
    skirmish_->world().set_attack_windup(true);
    skirmish_->world().set_projectile_travel(true);
    skirmish_->world().set_death_reveal(true);
    for (int p = 0; p < 2; ++p) {
        const RaceUnits u = race_units(rs2[p]);
        skirmish_->set_spawn_models(p, models_.model_for(u.worker), models_.model_for(u.army),
                                    models_.model_for(u.producer));
    }
    state_ = State::Skirmish;
}

bool FrontEnd::step(const render::InputState& in, float dt_ms,
                    std::vector<render::SceneInstance>& scene, render::Camera& cam) {
    if (state_ == State::Skirmish && skirmish_) {
        if (in.quit) { skirmish_.reset(); real_map_.reset(); state_ = State::Lobby;
                       prev_left_ = false; scene.clear(); cam = menu_ctl_.camera(); return true; }
        return skirmish_->step(in, dt_ms, scene, cam);
    }
    if (state_ == State::Sandbox && sandbox_) {
        if (in.quit) { sandbox_.reset(); state_ = State::Menu; scene.clear();
                       cam = menu_ctl_.camera(); return true; }
        return sandbox_->step(in, dt_ms, scene, cam);
    }

    const bool click = in.left_pressed && !prev_left_ && in.view_w && in.view_h;
    if (state_ == State::Menu) {
        if (click)
            for (const Button& b : menu_buttons(in.view_w, in.view_h))
                if (b.hit(in.mouse_x, in.mouse_y)) {
                    if (b.action == 1) state_ = State::Lobby;
                    else if (b.action == 2) start_sandbox();
                    break;
                }
        if (in.quit) { prev_left_ = in.left_pressed; return false; }
    } else if (state_ == State::Lobby) {
        if (in.quit) { state_ = State::Menu; }
        else if (click)
            for (const Button& b : lobby_buttons(in.view_w, in.view_h)) {
                if (!b.hit(in.mouse_x, in.mouse_y)) continue;
                if (b.action >= kMapRow0) sel_map_ = b.action - kMapRow0;
                else if (b.action == kActP0Race) p0_race_ = race_cycle(p0_race_);
                else if (b.action == kActP1Race) p1_race_ = race_cycle(p1_race_);
                else if (b.action == kActMapsFolder) { if (maps_folder_cb_) maps_folder_cb_(); }
                else if (b.action == kActBack) state_ = State::Menu;
                else if (b.action == kActStart) start_lobby_match();
                break;
            }
    }
    prev_left_ = in.left_pressed;

    scene.clear();
    cam = menu_ctl_.camera();
    return true;
}

void FrontEnd::build_overlay(std::uint32_t view_w, std::uint32_t view_h,
                             std::vector<std::uint8_t>& out) {
    if (state_ == State::Skirmish && skirmish_) { skirmish_->build_overlay(view_w, view_h, out); return; }
    if (state_ == State::Sandbox && sandbox_) { sandbox_->build_overlay(view_w, view_h, out); return; }

    out.assign(std::size_t(view_w) * view_h * 4, 0);
    if (!view_w || !view_h) return;
    draw::fill(out, view_w, view_h, 0, 0, int(view_w), int(view_h), 8, 10, 16, 150);

    if (state_ == State::Menu) {
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
        return;
    }

    const std::string title = "MELEE  -  SET UP GAME";
    draw::text(out, view_w, view_h, 48, 56, title, 4, 120, 200, 255);
    draw::text(out, view_w, view_h, 48, 104, "MAP", 2, 150, 165, 185);
    draw::text(out, view_w, view_h, int(view_w) / 2 + 24, 104, "PLAYERS", 2, 150, 165, 185);

    for (const Button& b : lobby_buttons(view_w, view_h)) {
        bool is_map = b.action >= kMapRow0;
        bool selected = is_map && (b.action - kMapRow0) == sel_map_;
        bool is_start = b.action == kActStart;
        std::uint8_t br = selected ? 40 : 22, bg = selected ? 60 : 30, bb = selected ? 92 : 48;
        if (is_start) { br = 30; bg = 70; bb = 40; }
        draw::fill(out, view_w, view_h, b.x0, b.y0, b.x1, b.y1, br, bg, bb, 235);
        draw::box(out, view_w, view_h, b.x0, b.y0, b.x1, b.y1,
                  selected || is_start ? 120 : 80, selected || is_start ? 200 : 110, 220);
        int sc = is_map ? 2 : 3;
        int ty = b.y0 + (b.y1 - b.y0 - ui::kFontH * sc) / 2;
        int tx = is_map ? b.x0 + 12 : b.x0 + (b.x1 - b.x0 - draw::text_w(b.label, sc)) / 2;
        draw::text(out, view_w, view_h, tx, ty, b.label, sc, 210, 225, 245);
    }

    if (int(maps_.size()) > 1) {
        std::string mp = "maps found: " + std::to_string(maps_.size() - 1);
        draw::text(out, view_w, view_h, 48, int(view_h) - 120, mp, 2, 120, 135, 155);
    } else {
        draw::text(out, view_w, view_h, 48, int(view_h) - 120,
                   "no .SC2Map found - playing the built-in arena", 2, 150, 130, 110);
    }
    draw::text(out, view_w, view_h, 48, int(view_h) - 40, "Esc  back", 2, 130, 140, 155);
}

}
