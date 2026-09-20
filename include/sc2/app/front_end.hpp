#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "sc2/app/cliff_place.hpp"
#include "sc2/app/glue_screen.hpp"
#include "sc2/app/melee_game.hpp"
#include "sc2/app/menu.hpp"
#include "sc2/app/platform.hpp"
#include "sc2/app/sandbox_game.hpp"
#include "sc2/app/screen_router.hpp"
#include "sc2/app/test_map.hpp"
#include "sc2/app/ui_theme.hpp"
#include "sc2/data/environment.hpp"
#include "sc2/formats/map_archive.hpp"
#include "sc2/render/camera_control.hpp"
#include "sc2/render/font_ttf.hpp"
#include "sc2/render/input.hpp"
#include "sc2/render/scene.hpp"

namespace sc2::casc {
class Storage;
}
namespace sc2::data {
class Catalog;
}

namespace sc2::app {

struct ScoreRow {
    std::string id;
    std::string publish_name;
    std::int32_t value[2] = {0, 0};
};

class FrontEnd {
public:
    FrontEnd(data::Catalog& cat, MeleeModels& models, casc::Storage& store, const TestMap& map);
    ~FrontEnd() {
        if (draw::active_font() == &overlay_font_ || draw::active_font() == &overlay_font_ext_) draw::set_active_font(nullptr);
    }

    void resolve_shell_faces() { ensure_overlay_font(); }

    static const std::vector<const char*>& options_panels();
    int options_panel() const { return options_panel_; }
    void select_options_panel(int i);
    using ConfigGet = std::function<std::string(const std::string&)>;
    using ConfigSet = std::function<bool(const std::string&, const std::string&)>;
    void set_config_io(ConfigGet get, ConfigSet set) { config_get_ = std::move(get); config_set_ = std::move(set); }
    static std::string option_key(const std::string& checkbox_name);
    bool option_checked(const std::string& checkbox_name) const;
    void toggle_option(const std::string& checkbox_name);
    const std::string& shell_face_path(theme::Face f) const { return face_path_[std::size_t(f)]; }
    const render::OverlayFont& shell_face(theme::Face f) const { return f == theme::Face::HeaderExtended ? overlay_font_ext_ : overlay_font_; }

    void set_maps(std::vector<platform::MapEntry> maps);

    void set_maps_folder_callback(std::function<void()> cb) { maps_folder_cb_ = std::move(cb); }

    void set_console_renderer(MeleeGame::ConsoleRenderer fn) { console_renderer_ = std::move(fn); }
    void set_portrait_renderer(MeleeGame::PortraitRenderer fn) { portrait_renderer_ = std::move(fn); }

    void set_texture_callback(std::function<void(formats::Model&, const formats::LoadedMap&)> cb) {
        texture_cb_ = std::move(cb);
    }

    bool step(const render::InputState& in, float dt_ms,
              std::vector<render::SceneInstance>& scene, render::Camera& cam);
    void build_overlay(std::uint32_t view_w, std::uint32_t view_h, std::vector<std::uint8_t>& out);

    const data::EnvLight& current_environment() const { return current_env_; }
    void set_ground_fog(bool on) { ground_fog_ = on; if (skirmish_) skirmish_->set_ground_fog(on); }
    const render::FogField* ground_fog() const { return skirmish_ ? skirmish_->ground_fog() : nullptr; }

    void enter_sandbox() { start_sandbox(); }
    void enter_skirmish() { start_skirmish(); }
    void enter_replays() { router_.replace(ScreenId::ReplayBrowse); }

    struct ReplayEntry {
        std::string path;
        std::string name;
    };
    void set_replays_dir(std::string dir) {
        replays_dir_ = std::move(dir);
        replays_scanned_ = false;
    }
    const std::string& replays_dir() const { return replays_dir_; }
    void set_saves_dir(std::string dir) { saves_dir_ = std::move(dir); }
    const std::string& saves_dir() const { return saves_dir_; }
    std::string save_game();
    bool load_save(const std::string& path);
    const std::vector<ReplayEntry>& replays() const { return replays_; }
    int selected_replay_index() const { return sel_replay_; }
    bool replay_pending() const { return replay_pending_; }
    const std::string& last_recorded_replay() const { return last_recorded_replay_; }
    void enter_lobby() { router_.replace(ScreenId::Lobby); }

    bool begin_match(std::vector<std::uint8_t> map_bytes, Race p0, Race p1) {
        api_map_bytes_ = std::move(map_bytes);
        p0_race_ = p0;
        p1_race_ = p1;
        return begin_match();
    }
    bool begin_match();
    void end_match() {
        release_match();
        router_.unwind_to(ScreenId::GlueRoot);
        prev_left_ = false;
    }
    MatchResult::Outcome last_outcome() const { return last_outcome_; }
    const std::vector<ScoreRow>& last_score() const { return last_score_; }
    void clear_score() { last_score_.clear(); }
    void draw_score_table(std::uint32_t view_w, std::uint32_t view_h, std::vector<std::uint8_t>& out) const;
    MeleeGame* active_match() { return skirmish_.get(); }
    const formats::LoadedMap* active_map() const { return real_map_.get(); }

    void set_menu_data_dir(std::string dir) {
        menu_data_dir_ = std::move(dir);
        real_menu_.reset();
        real_menu_tried_ = false;
    }

    void force_menu_for_test(MainMenu m) {
        real_menu_ = std::move(m);
        real_menu_forced_ = true;
        real_menu_tried_ = true;
    }
    void disable_real_menu_for_test() { real_menu_disabled_ = true; }
    bool showing_menu() const { return router_.top() == ScreenId::GlueRoot; }
    bool showing_lobby() const { return router_.top() == ScreenId::Lobby; }
    bool showing_sandbox() const { return router_.top() == ScreenId::Sandbox; }
    bool showing_skirmish() const { return router_.top() == ScreenId::InGame; }
    bool showing_loading() const { return router_.top() == ScreenId::Loading; }

    const ScreenRouter& router() const { return router_; }
    ScreenId current_screen() const { return router_.top(); }
    std::vector<std::pair<std::string, ui::Rect>> screen_controls(std::uint32_t view_w,
                                                                  std::uint32_t view_h);
    bool current_screen_is_authored(std::uint32_t view_w, std::uint32_t view_h);

    const std::vector<platform::MapEntry>& lobby_maps() const { return maps_; }
    int selected_map_index() const { return sel_map_; }
    const std::string& match_map_path() const { return match_map_path_; }
    const std::string& last_match_error() const { return match_error_; }
    int map_scroll() const { return map_scroll_; }

private:
    struct Button {
        int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
        std::string label;
        std::string trigger;
        int action = 0;
        bool hit(float mx, float my) const {
            return mx >= float(x0) && mx < float(x1) && my >= float(y0) && my < float(y1);
        }
    };
    std::vector<Button> menu_buttons(std::uint32_t view_w, std::uint32_t view_h);
    std::vector<Button> lobby_buttons(std::uint32_t view_w, std::uint32_t view_h) const;

    const GlueScreen* glue_screen(ScreenId id, std::uint32_t view_w, std::uint32_t view_h);
    void draw_glue_screen(ScreenId id, std::uint32_t view_w, std::uint32_t view_h,
                          std::vector<std::uint8_t>& out);
    void click_glue_screen(ScreenId id, float mx, float my, std::uint32_t view_w,
                           std::uint32_t view_h);

    const MainMenu* real_menu(std::uint32_t view_w, std::uint32_t view_h);
    void start_skirmish();
    void start_sandbox();
    std::vector<Button> replay_buttons(std::uint32_t view_w, std::uint32_t view_h) const;
    void scan_replays();
    void record_match_replay();
    int lobby_rows_per_page(std::uint32_t view_h) const;
    void clamp_map_scroll(std::uint32_t view_h);

    data::Catalog& cat_;
    MeleeModels& models_;
    casc::Storage& store_;
    const TestMap& map_;

    ScreenRouter router_;
    void release_match() {
        capture_score();
        skirmish_.reset();
        real_map_.reset();
        real_ground_ = formats::Model{};
        real_doodads_.clear();
        real_cliffs_ = CliffField{};
        map_built_ = false;
        pending_map_bytes_.clear();
        load_plan_.reset();
    }
    MeleeGame::ConsoleRenderer console_renderer_;
    MeleeGame::PortraitRenderer portrait_renderer_;
    std::unique_ptr<MeleeGame> skirmish_;
    bool ground_fog_ = false;
    MatchResult::Outcome last_outcome_ = MatchResult::Outcome::Ongoing;
    std::unique_ptr<data::UpgradeCatalog> research_up_;
    std::unique_ptr<SandboxGame> sandbox_;
    render::CameraControl menu_ctl_;
    bool prev_left_ = false;

    std::string menu_data_dir_;
    std::optional<MainMenu> real_menu_;
    std::uint32_t real_menu_w_ = 0, real_menu_h_ = 0;
    bool real_menu_tried_ = false;
    bool real_menu_forced_ = false;
    bool real_menu_disabled_ = false;

    struct GlueCacheEntry {
        std::optional<GlueScreen> screen;
        std::uint32_t w = 0, h = 0;
        bool tried = false;
    };
    std::vector<GlueCacheEntry> glue_cache_{std::size_t(ScreenId::Count)};
    int options_panel_ = 0;
    std::string replays_dir_;
    std::vector<ReplayEntry> replays_;
    int sel_replay_ = -1;
    bool replays_scanned_ = false;
    bool replay_pending_ = false;
    OrderLog replay_log_pending_;
    bool resume_pending_ = false;
    std::string load_pending_path_;
    std::string newest_save() const;
    std::string saves_dir_;
    std::string replay_name_;
    std::string last_recorded_replay_;

    int loading_frames_ = 0;

public:
    struct LoadStep {
        enum class Kind { MapBytes, MapTerrain, MapCliffs, MapMesh, Model, Layout, Console, Font,
                          Icon };
        Kind kind = Kind::MapBytes;
        std::string arg;
        std::string label;
    };
    struct LoadPlan {
        std::vector<LoadStep> steps;
        std::size_t done = 0;
        bool planned = false;
        std::string label;
        int warmed = 0;
        int missing = 0;
        float frac() const {
            return steps.empty() ? 0.0f : float(done) / float(steps.size());
        }
        void reset() { *this = LoadPlan{}; }
    };
    const LoadPlan& load_plan() const { return load_plan_; }

private:
    LoadPlan load_plan_;
    void plan_load();
    bool step_load(std::uint32_t view_w, std::uint32_t view_h);
    bool build_real_map_phase(int phase);
    void invalidate_map_if_changed(const std::string& path);
    bool map_built_ = false;
    std::string built_map_path_;
    std::vector<std::uint8_t> pending_map_bytes_;

    std::vector<platform::MapEntry> maps_;
    int sel_map_ = 0;
    int map_scroll_ = 0;
    std::string match_map_path_;
    std::string match_error_;
    std::vector<ScoreRow> last_score_;
    void capture_score();
    Race p0_race_ = Race::Terran;
    Race p1_race_ = Race::Terran;
    LobbyRace p0_pick_ = LobbyRace::Terran, p1_pick_ = LobbyRace::Terran;

    std::unique_ptr<formats::LoadedMap> real_map_;
    std::vector<std::uint8_t> api_map_bytes_;
    data::EnvLight current_env_;
    formats::Model real_ground_;
    formats::Model real_water_;
    std::array<float, 4> real_water_rgba_ = {1.0f, 1.0f, 1.0f, 1.0f};
    std::vector<formats::PlacedDoodad> real_doodads_;
    CliffField real_cliffs_;

    std::function<void()> maps_folder_cb_;
    std::function<void(formats::Model&, const formats::LoadedMap&)> texture_cb_;

    render::OverlayFont overlay_font_;
    render::OverlayFont overlay_font_ext_;
    std::map<std::string, theme::Rgb> style_rgb_;
    ConfigGet config_get_;
    ConfigSet config_set_;
    std::map<std::string, std::string> config_mem_;
    std::string face_path_[2];
    bool overlay_font_tried_ = false;
    void ensure_overlay_font();
};

}
