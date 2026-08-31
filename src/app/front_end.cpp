#include "sc2/app/front_end.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iterator>

#include "sc2/app/ability.hpp"
#include "sc2/app/map_doodads.hpp"
#include "sc2/app/overlay_draw.hpp"
#include "sc2/app/race_melee_setup.hpp"
#include "sc2/app/shipping_wiring.hpp"
#include "sc2/app/spawn.hpp"
#include "sc2/app/water_heights.hpp"
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

constexpr int kActStart = 1, kActBack = 2, kActMapsFolder = 3, kActScrollUp = 4,
              kActScrollDown = 5, kActP0Race = 10, kActP1Race = 11, kMapRow0 = 100;

constexpr const char* kMainMenuLayoutPath =
    "mods/core.sc2mod/base.sc2data/UI/Layout/Glue/ScreenNavigationSC2.SC2Layout";
}

FrontEnd::FrontEnd(data::Catalog& cat, MeleeModels& models, casc::Storage& store,
                   const TestMap& map)
    : cat_(cat), models_(models), store_(store), map_(map) {
    menu_ctl_.focus = {map_.width * 0.5f, map_.height * 0.5f, 0.0f};
    menu_ctl_.distance = 48.0f;
    menu_ctl_.yaw = 1.5707963f;
    menu_ctl_.pitch = 1.0f;
    menu_ctl_.mirror_x = true;
    maps_.push_back({"", "Synthetic Arena (72x72)  [built-in, no cliffs]"});
}

void FrontEnd::set_maps(std::vector<platform::MapEntry> maps) {
    maps_.assign(1, platform::MapEntry{"", "Synthetic Arena (72x72)  [built-in, no cliffs]"});
    for (auto& m : maps) maps_.push_back(std::move(m));
    sel_map_ = maps_.size() > 1 ? 1 : 0;
    map_scroll_ = 0;
}

int FrontEnd::lobby_rows_per_page(std::uint32_t view_h) const {
    return std::max(1, (int(view_h) - 132 - 140) / 34);
}

void FrontEnd::clamp_map_scroll(std::uint32_t view_h) {
    const int page = lobby_rows_per_page(view_h);
    const int max_scroll = std::max(0, int(maps_.size()) - page);
    map_scroll_ = std::max(0, std::min(map_scroll_, max_scroll));
}

const MainMenu* FrontEnd::real_menu(std::uint32_t view_w, std::uint32_t view_h) {
    if (real_menu_disabled_ || !view_w || !view_h) return nullptr;
    if (real_menu_forced_) return real_menu_ ? &*real_menu_ : nullptr;
    if (real_menu_tried_ && real_menu_w_ == view_w && real_menu_h_ == view_h)
        return real_menu_ ? &*real_menu_ : nullptr;
    real_menu_tried_ = true;
    real_menu_w_ = view_w;
    real_menu_h_ = view_h;
    real_menu_.reset();

    std::string dir = menu_data_dir_;
    if (dir.empty())
        if (const char* env = std::getenv("SC2_DATA_DIR")) dir = env;
    if (dir.empty()) return nullptr;

    const ui::Rect canvas{0, 0, float(view_w), float(view_h)};
    std::optional<MainMenu> m = MainMenu::from_store(store_, kMainMenuLayoutPath, canvas);
    if (!m) return nullptr;

    const float canvas_area = float(view_w) * float(view_h);
    bool usable = false;
    for (const ui::SolvedFrame& sf : m->buttons()) {
        if (MainMenu::classify(sf.name) == MenuAction::None) continue;
        if (sf.rect.w <= 1.0f || sf.rect.h <= 1.0f) continue;
        if (sf.rect.w * sf.rect.h < 0.9f * canvas_area) { usable = true; break; }
    }
    if (!usable) return nullptr;
    real_menu_ = std::move(m);
    return &*real_menu_;
}

std::vector<FrontEnd::Button> FrontEnd::menu_buttons(std::uint32_t view_w, std::uint32_t view_h) {
    std::vector<Button> out;
    if (const MainMenu* m = real_menu(view_w, view_h)) {
        out.reserve(m->buttons().size());
        for (std::size_t i = 0; i < m->buttons().size(); ++i) {
            const ui::Rect& r = m->hit_rect(i);
            const std::string& name = m->buttons()[i].name;
            out.push_back({int(r.x), int(r.y), int(r.x + r.w), int(r.y + r.h), name, name, 0});
        }
    }

    struct OwnEntry { const char* label; const char* trigger; };
    static const OwnEntry kOwn[] = {
        {"1v1 Melee  vs AI", "PlayButton"},
        {"Sandbox  Test Map", "SandboxButton"},
        {"Options", "menu/Options"},
        {"Credits", "Credits entry"},
    };
    const int bw = 380, bh = 54, gap = 18;
    const int x0 = (int(view_w) - bw) / 2;
    int y = int(view_h) / 2 - bh;
    for (const OwnEntry& e : kOwn) {
        bool already = false;
        for (const Button& b : out)
            if (trigger_for_control(ScreenId::GlueRoot, b.trigger) == e.trigger) already = true;
        if (already) continue;
        out.push_back({x0, y, x0 + bw, y + bh, e.label, e.trigger, 0});
        y += bh + gap;
    }
    return out;
}

const GlueScreen* FrontEnd::glue_screen(ScreenId id, std::uint32_t view_w, std::uint32_t view_h) {
    if (id >= ScreenId::Count || !view_w || !view_h) return nullptr;
    GlueCacheEntry& e = glue_cache_[std::size_t(id)];
    if (e.tried && e.w == view_w && e.h == view_h) return e.screen ? &*e.screen : nullptr;
    e.tried = true;
    e.w = view_w;
    e.h = view_h;
    e.screen.reset();
    const std::vector<const char*>& docs = screen_layouts(id);
    if (docs.empty()) return nullptr;
    std::vector<std::string> paths(docs.begin(), docs.end());
    e.screen = GlueScreen::harvest(store_, paths, ui::Rect{0, 0, float(view_w), float(view_h)});
    return e.screen ? &*e.screen : nullptr;
}

void FrontEnd::click_glue_screen(ScreenId id, float mx, float my, std::uint32_t view_w,
                                 std::uint32_t view_h) {
    const GlueScreen* s = glue_screen(id, view_w, view_h);
    if (!s) return;
    const ui::SolvedFrame* f = s->hit(mx, my);
    if (!f) return;
    router_.follow(trigger_for_control(id, f->name));
}

std::vector<FrontEnd::Button> FrontEnd::lobby_buttons(std::uint32_t view_w,
                                                      std::uint32_t view_h) const {
    std::vector<Button> out;
    const int W = int(view_w), H = int(view_h);
    const int lx0 = 48, lx1 = W / 2 - 24, rowh = 30, gap = 4, listy = 132;
    const int page = lobby_rows_per_page(view_h);
    const int first = map_scroll_;
    for (int i = 0; i < page && first + i < int(maps_.size()); ++i) {
        int y = listy + i * (rowh + gap);
        out.push_back({lx0, y, lx1, y + rowh, maps_[first + i].name, "", kMapRow0 + first + i});
    }
    if (int(maps_.size()) > page) {
        const int sy = listy + page * (rowh + gap) + 8, sw = (lx1 - lx0 - 12) / 2;
        out.push_back({lx0, sy, lx0 + sw, sy + 36, "^  Prev", "", kActScrollUp});
        out.push_back({lx1 - sw, sy, lx1, sy + 36, "v  Next", "", kActScrollDown});
    }
    const int rx0 = W / 2 + 24, rx1 = W - 48, rbh = 48;
    out.push_back({rx0, listy + 24, rx1, listy + 24 + rbh,
                   std::string("Your Race:  ") + race_name(p0_race_), "", kActP0Race});
    out.push_back({rx0, listy + 24 + rbh + 20, rx1, listy + 24 + 2 * rbh + 20,
                   std::string("Opponent:  ") + race_name(p1_race_), "", kActP1Race});
    if (maps_folder_cb_)
        out.push_back({rx0, listy + 24 + 2 * rbh + 60, rx1, listy + 24 + 3 * rbh + 60,
                       "Maps Folder...", "", kActMapsFolder});
    const int bw = 200, bh = 52;
    int by = H - 96;
    out.push_back({W / 2 + 12, by, W / 2 + 12 + bw, by + bh, "START", "", kActStart});
    out.push_back({W / 2 - 12 - bw, by, W / 2 - 12, by + bh, "Back", "", kActBack});
    return out;
}

void FrontEnd::start_sandbox() {
    sandbox_ = std::make_unique<SandboxGame>(cat_, models_, store_, map_);
    router_.replace(ScreenId::Sandbox);
}

void FrontEnd::start_skirmish() {
    p0_race_ = Race::Terran;
    p1_race_ = Race::Terran;
    sel_map_ = 0;
    begin_match();
}

bool FrontEnd::begin_match() {
    match_error_.clear();
    const bool synthetic = api_map_bytes_.empty() && (sel_map_ <= 0 || sel_map_ >= int(maps_.size()) ||
                                                      maps_[sel_map_].path.empty());
    match_map_path_ = synthetic || !api_map_bytes_.empty() ? std::string() : maps_[sel_map_].path;
    MeleeSetup setup;
    const formats::Model* ground = nullptr;
    float map_w = 0, map_h = 0;
    const sim::NavGrid* nav = nullptr;
    const sim::TerrainMap* placement = nullptr;
    float playable_x0 = -1.0f, playable_y0 = -1.0f, playable_x1 = -1.0f, playable_y1 = -1.0f;

    if (synthetic) {
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
    } else try {
        std::vector<std::uint8_t> bytes;
        if (!api_map_bytes_.empty()) {
            bytes = std::move(api_map_bytes_);
            api_map_bytes_.clear();
        } else if (maps_[sel_map_].casc) {
            bytes = store_.read_by_name(maps_[sel_map_].path);
        } else {
            std::ifstream in(maps_[sel_map_].path, std::ios::binary);
            bytes.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        }
        real_map_ = std::make_unique<formats::LoadedMap>(
            formats::load_map_terrain(std::move(bytes)));
        app::stamp_map_doodads(cat_, *real_map_);
        app::stamp_map_units(cat_, *real_map_);
        current_env_ = data::resolve_environment_light(cat_, real_map_->tileset);
        real_cliffs_ = app::place_cliffs(store_, *real_map_);
        const bool have_cliffs = !real_cliffs_.instances.empty();
        real_ground_ = formats::build_map_mesh_stepped(*real_map_, 1.0f, 128, !have_cliffs);
        if (texture_cb_) texture_cb_(real_ground_, *real_map_);
        const auto water_heights = water_type_heights(store_);
        formats::append_map_water(real_ground_, *real_map_, 1.0f, &water_heights);
        setup = melee_setup_for_map(*real_map_, cat_, p0_race_, p1_race_);
        ground = &real_ground_;
        map_w = float(real_map_->width);
        map_h = float(real_map_->height);
        nav = &real_map_->nav;
        placement = &real_map_->placement;
        if (real_map_->info.has_playable_area()) {
            playable_x0 = float(real_map_->info.playable_x0);
            playable_y0 = float(real_map_->info.playable_y0);
            playable_x1 = float(real_map_->info.playable_x1);
            playable_y1 = float(real_map_->info.playable_y1);
        }
    } catch (const std::exception& e) {
        const std::string which = sel_map_ >= 0 && sel_map_ < int(maps_.size())
                                      ? maps_[sel_map_].name
                                      : std::string("the supplied archive");
        match_error_ = "cannot load \"" + which + "\": " + e.what();
        real_map_.reset();
        real_cliffs_ = CliffField{};
        real_doodads_.clear();
        api_map_bytes_.clear();
        router_.replace(ScreenId::Lobby);
        return false;
    }

    const Race rs2[2] = {p0_race_, p1_race_};
    for (int p = 0; p < 2; ++p) {
        const RaceUnits u = race_units(rs2[p]);
        for (const char* tok : {u.hall, u.worker, u.producer, u.supply, u.army})
            models_.ensure(store_, tok);
    }
    for (const sim::Unit& n : setup.neutrals)
        if (!n.catalog_id.empty()) models_.ensure(store_, n.catalog_id);

    MeleeGameConfig cfg;
    cfg.human_player = 0;
    skirmish_ = std::make_unique<MeleeGame>(cfg);
    skirmish_->set_ui_storage(&store_);
    skirmish_->set_build_verb(p0_race_ == Race::Protoss ? sim::BuildVerb::Warp
                              : p0_race_ == Race::Zerg  ? sim::BuildVerb::Morph
                                                        : sim::BuildVerb::Committed);
    skirmish_->register_creep_masks(cat_);
    if (real_map_) skirmish_->set_tier_heights(formats::tier_heights(*real_map_));
    skirmish_->setup(setup, &models_, ground, map_w, map_h, nav, placement, playable_x0,
                     playable_y0, playable_x1, playable_y1);
    if (real_map_) {
        real_doodads_ = real_map_->objects.doodads;
        const auto& cm = real_map_->cliff;
        const std::vector<float> th = formats::tier_heights(*real_map_);
        for (formats::PlacedDoodad& d : real_doodads_) {
            const int cx = int(d.x), cy = int(d.y);
            if (!th.empty() && !cm.empty() && cx >= 0 && cy >= 0 && std::uint32_t(cx) < cm.width &&
                std::uint32_t(cy) < cm.height) {
                const std::size_t L = std::size_t(cm.levels[std::size_t(cy) * cm.width + cx]);
                d.z = th[L < th.size() ? L : th.size() - 1];
            } else {
                d.z = 0.0f;
            }
        }
        skirmish_->set_doodads(&real_doodads_);
        skirmish_->set_cliffs(&real_cliffs_.instances);
    }
    if (!research_up_) {
        research_up_ = std::make_unique<data::UpgradeCatalog>();
        try {
            research_up_->load_melee(store_);
        } catch (const std::exception&) {
        }
    }
    const std::vector<Race> races = {p0_race_, p1_race_};
    apply_shipping_wiring(*skirmish_, cat_, research_up_.get(), &setup, &races);
    for (int p = 0; p < 2; ++p) {
        const RaceUnits u = race_units(rs2[p]);
        skirmish_->set_spawn_models(p, models_.model_for(u.worker), models_.model_for(u.army),
                                    models_.model_for(u.producer));
    }
    if (router_.top() == ScreenId::Loading)
        router_.follow("load complete");
    else
        router_.replace(ScreenId::InGame);
    return true;
}

bool FrontEnd::step(const render::InputState& in, float dt_ms,
                    std::vector<render::SceneInstance>& scene, render::Camera& cam) {
    if (skirmish_) {
        const bool open = skirmish_->pause_menu_open();
        if (open && router_.top() == ScreenId::InGame)
            router_.follow("Esc / F10");
        else if (!open && router_.top() == ScreenId::PauseMenu)
            router_.follow("Resume");
    }
    if ((router_.top() == ScreenId::InGame || router_.top() == ScreenId::PauseMenu) && skirmish_) {
        if (!skirmish_->step(in, dt_ms, scene, cam)) {
            skirmish_.reset(); real_map_.reset();
            if (router_.top() == ScreenId::PauseMenu) router_.follow("Quit to Main Menu");
            else router_.unwind_to(ScreenId::GlueRoot);
            prev_left_ = false; scene.clear(); cam = menu_ctl_.camera();
            return true;
        }
        if (router_.top() == ScreenId::InGame &&
            skirmish_->outcome() != MatchResult::Outcome::Ongoing) {
            last_outcome_ = skirmish_->outcome();
            release_match();
            router_.follow("win/lose condition met");
            prev_left_ = in.left_pressed;
            scene.clear();
            cam = menu_ctl_.camera();
        }
        return true;
    }
    if (router_.top() == ScreenId::Sandbox && sandbox_) {
        if (!sandbox_->step(in, dt_ms, scene, cam)) {
            sandbox_.reset(); router_.follow("Leave sandbox"); scene.clear();
            cam = menu_ctl_.camera();
        }
        return true;
    }
    if (router_.top() == ScreenId::Loading) {
        if (in.quit) {
            router_.cancel();
            loading_frames_ = 0;
        } else if (loading_frames_ >= 1) {
            loading_frames_ = 0;
            begin_match();
        } else {
            ++loading_frames_;
        }
        prev_left_ = in.left_pressed;
        scene.clear();
        cam = menu_ctl_.camera();
        return true;
    }

    const bool click = in.left_pressed && !prev_left_ && in.view_w && in.view_h;
    if (router_.top() != ScreenId::GlueRoot && router_.top() != ScreenId::Lobby) {
        if (in.quit) {
            if (!router_.cancel()) { prev_left_ = in.left_pressed; return false; }
        } else if (click) {
            click_glue_screen(router_.top(), in.mouse_x, in.mouse_y, in.view_w, in.view_h);
        }
        if (router_.top() == ScreenId::Boot) { prev_left_ = in.left_pressed; return false; }
        if (router_.top() == ScreenId::Loading) loading_frames_ = 0;
        prev_left_ = in.left_pressed;
        scene.clear();
        cam = menu_ctl_.camera();
        return true;
    }
    if (router_.top() == ScreenId::GlueRoot) {
        if (click) {
            const std::vector<Button> bs = menu_buttons(in.view_w, in.view_h);
            for (auto it = bs.rbegin(); it != bs.rend(); ++it)
                if (it->hit(in.mouse_x, in.mouse_y)) {
                    router_.follow(trigger_for_control(ScreenId::GlueRoot, it->trigger));
                    break;
                }
        } else if (in.quit)
            router_.cancel();
        if (router_.top() == ScreenId::Sandbox && !sandbox_) start_sandbox();
        if (router_.top() == ScreenId::Boot) { prev_left_ = in.left_pressed; return false; }
    } else if (router_.top() == ScreenId::Lobby) {
        if (in.view_h) {
            if (in.wheel != 0.0f) map_scroll_ -= int(in.wheel > 0.0f ? 1 : -1) * 3;
            clamp_map_scroll(in.view_h);
        }
        if (in.quit) { router_.cancel(); }
        else if (click)
            for (const Button& b : lobby_buttons(in.view_w, in.view_h)) {
                if (!b.hit(in.mouse_x, in.mouse_y)) continue;
                if (b.action >= kMapRow0) { sel_map_ = b.action - kMapRow0; match_error_.clear(); }
                else if (b.action == kActP0Race) p0_race_ = race_cycle(p0_race_);
                else if (b.action == kActP1Race) p1_race_ = race_cycle(p1_race_);
                else if (b.action == kActMapsFolder) { if (maps_folder_cb_) maps_folder_cb_(); }
                else if (b.action == kActScrollUp) { map_scroll_ -= lobby_rows_per_page(in.view_h); clamp_map_scroll(in.view_h); }
                else if (b.action == kActScrollDown) { map_scroll_ += lobby_rows_per_page(in.view_h); clamp_map_scroll(in.view_h); }
                else if (b.action == kActBack) router_.follow("Back / leave lobby");
                else if (b.action == kActStart) {
                    match_error_.clear();
                    loading_frames_ = 0;
                    router_.follow("StartButton -> countdown");
                }
                break;
            }
    }
    prev_left_ = in.left_pressed;

    scene.clear();
    cam = menu_ctl_.camera();
    return true;
}

void FrontEnd::ensure_overlay_font() {
    if (overlay_font_tried_) {
        if (overlay_font_.valid()) draw::set_active_font(&overlay_font_);
        return;
    }
    overlay_font_tried_ = true;
    static const char* kCandidates[] = {
        "mods/core.sc2mod/base.sc2data/UI/Fonts/Eurostile-Reg.otf",
        "mods/core.sc2mod/base.sc2data/UI/Fonts/Eurostile-Bol.otf",
        "mods/core.sc2mod/base.sc2data/UI/Fonts/BLQ55.ttf",
        "mods/core.sc2mod/base.sc2data/UI/Fonts/BLQ85.ttf",
        "mods/core.sc2mod/base.sc2data/UI/Fonts/bl.ttf",
    };
    for (const char* path : kCandidates) {
        try {
            auto bytes = store_.read_by_name(path);
            if (overlay_font_.load(bytes)) { draw::set_active_font(&overlay_font_); return; }
        } catch (const std::exception&) {   }
    }
}

std::vector<std::pair<std::string, ui::Rect>> FrontEnd::screen_controls(std::uint32_t view_w,
                                                                       std::uint32_t view_h) {
    std::vector<std::pair<std::string, ui::Rect>> out;
    const ScreenId id = router_.top();
    auto push_buttons = [&out](const std::vector<Button>& bs) {
        for (const Button& b : bs)
            out.push_back({b.trigger.empty() ? b.label : b.trigger,
                           ui::Rect{float(b.x0), float(b.y0), float(b.x1 - b.x0),
                                    float(b.y1 - b.y0)}});
    };
    if (id == ScreenId::GlueRoot) {
        push_buttons(menu_buttons(view_w, view_h));
        return out;
    }
    if (id == ScreenId::Lobby) {
        push_buttons(lobby_buttons(view_w, view_h));
        return out;
    }
    if (const GlueScreen* s = glue_screen(id, view_w, view_h))
        for (std::size_t i = 0; i < s->controls().size(); ++i)
            out.push_back({s->controls()[i].name, s->hit_rect(i)});
    return out;
}

bool FrontEnd::current_screen_is_authored(std::uint32_t view_w, std::uint32_t view_h) {
    const ScreenId id = router_.top();
    if (id == ScreenId::GlueRoot) return real_menu(view_w, view_h) != nullptr;
    if (id == ScreenId::Lobby) return false;
    const GlueScreen* s = glue_screen(id, view_w, view_h);
    return s && !s->controls().empty();
}

void FrontEnd::draw_glue_screen(ScreenId id, std::uint32_t view_w, std::uint32_t view_h,
                                std::vector<std::uint8_t>& out) {
    const std::string title = screen_name(id);
    draw::text(out, view_w, view_h, 48, 56, title, 4, 120, 200, 255);
    if ((id == ScreenId::GameResult || id == ScreenId::Score) &&
        last_outcome_ != MatchResult::Outcome::Ongoing)
        draw::text(out, view_w, view_h, 48 + int(title.size()) * (ui::kFontW + 1) * 4 + 32, 56,
                   MatchResult::banner(last_outcome_), 4,
                   last_outcome_ == MatchResult::Outcome::Victory ? 120 : 235,
                   last_outcome_ == MatchResult::Outcome::Victory ? 235 : 120, 130);
    const GlueScreen* s = glue_screen(id, view_w, view_h);
    const std::vector<const char*>& docs = screen_layouts(id);
    if (!s || s->controls().empty()) {
        const std::string why =
            docs.empty()
                ? std::string("this screen has no authored layout document")
                : (!s ? std::string("this install did not supply ") + docs[0] + " and " +
                            std::to_string(docs.size() - 1) + " more"
                      : std::string("read ") + std::to_string(s->documents()) + " of " +
                            std::to_string(docs.size()) + " documents; the solver placed no "
                                                          "clickable control");
        draw::text(out, view_w, view_h, 48, 104, why, 2, 235, 170, 110);
        draw::text(out, view_w, view_h, 48, 132,
                   "nothing is drawn here rather than a made-up panel", 2, 150, 165, 185);
        draw::text(out, view_w, view_h, 48, int(view_h) - 40, "Esc  back", 2, 130, 140, 155);
        return;
    }
    const std::string src = std::to_string(s->controls().size()) + " authored controls from " +
                            std::to_string(s->documents()) + " of " + std::to_string(docs.size()) +
                            " documents (" + docs[0] + ")";
    draw::text(out, view_w, view_h, 48, 104, src, 2, 150, 165, 185);
    for (const ui::SolvedFrame& f : s->controls()) {
        const int x0 = int(f.rect.x), y0 = int(f.rect.y);
        const int x1 = int(f.rect.x + f.rect.w), y1 = int(f.rect.y + f.rect.h);
        if (x1 <= x0 || y1 <= y0) continue;
        const bool live = router_.edge_for(trigger_for_control(id, f.name)) != nullptr;
        draw::fill(out, view_w, view_h, x0, y0, x1, y1, live ? 24 : 18, live ? 34 : 22,
                   live ? 54 : 30, 225);
        draw::box(out, view_w, view_h, x0, y0, x1, y1, live ? 90 : 60, live ? 150 : 70,
                  live ? 220 : 90);
        draw::text(out, view_w, view_h, x0 + 6, y0 + (y1 - y0 - ui::kFontH * 2) / 2, f.name, 2,
                   live ? 200 : 120, live ? 220 : 130, live ? 245 : 145);
    }
    draw::text(out, view_w, view_h, 48, int(view_h) - 40, "Esc  back", 2, 130, 140, 155);
}

void FrontEnd::build_overlay(std::uint32_t view_w, std::uint32_t view_h,
                             std::vector<std::uint8_t>& out) {
    ensure_overlay_font();
    const ScreenId screen = router_.top();
    if ((screen == ScreenId::InGame || screen == ScreenId::PauseMenu) && skirmish_) {
        skirmish_->build_overlay(view_w, view_h, out);
        return;
    }
    if (screen == ScreenId::Sandbox && sandbox_) { sandbox_->build_overlay(view_w, view_h, out); return; }

    out.assign(std::size_t(view_w) * view_h * 4, 0);
    if (!view_w || !view_h) return;
    draw::fill(out, view_w, view_h, 0, 0, int(view_w), int(view_h), 8, 10, 16, 150);

    if (screen == ScreenId::Loading) {
        draw_glue_screen(screen, view_w, view_h, out);
        const std::string what =
            sel_map_ >= 0 && sel_map_ < int(maps_.size()) ? maps_[sel_map_].name : std::string("-");
        draw::text(out, view_w, view_h, 48, int(view_h) / 2 - 40, what, 4, 200, 220, 245);
        draw::text(out, view_w, view_h, 48, int(view_h) / 2 + 8,
                   "Reading map, building terrain, placing units...", 2, 150, 165, 185);
        draw::text(out, view_w, view_h, 48, int(view_h) / 2 + 36,
                   "progress bar pending: the load reports no stages yet (doc 24 s4)", 2, 130, 140,
                   155);
        return;
    }
    if (screen != ScreenId::GlueRoot && screen != ScreenId::Lobby) {
        draw_glue_screen(screen, view_w, view_h, out);
        return;
    }

    if (screen == ScreenId::GlueRoot) {
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
        const int page = lobby_rows_per_page(view_h);
        const int last = std::min(int(maps_.size()), map_scroll_ + page);
        std::string mp = "maps found: " + std::to_string(maps_.size() - 1) + "   showing " +
                         std::to_string(map_scroll_ + 1) + "-" + std::to_string(last) + " of " +
                         std::to_string(maps_.size()) + "   (wheel scrolls)";
        draw::text(out, view_w, view_h, 48, int(view_h) - 120, mp, 2, 120, 135, 155);
        const std::string sel =
            "selected: " + (sel_map_ >= 0 && sel_map_ < int(maps_.size()) ? maps_[sel_map_].name
                                                                         : std::string("-"));
        draw::text(out, view_w, view_h, 48, int(view_h) - 96, sel, 2, 150, 200, 240);
    } else {
        draw::text(out, view_w, view_h, 48, int(view_h) - 120,
                   "no .SC2Map found - playing the built-in arena", 2, 150, 130, 110);
    }
    if (!match_error_.empty())
        draw::text(out, view_w, view_h, 48, int(view_h) - 68, match_error_, 2, 235, 120, 100);
    draw::text(out, view_w, view_h, 48, int(view_h) - 40, "Esc  back", 2, 130, 140, 155);
}

}
