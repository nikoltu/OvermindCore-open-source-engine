#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "sc2/app/cliff_place.hpp"
#include "sc2/app/melee_game.hpp"
#include "sc2/app/menu.hpp"
#include "sc2/app/platform.hpp"
#include "sc2/app/sandbox_game.hpp"
#include "sc2/app/test_map.hpp"
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

class FrontEnd {
public:
    FrontEnd(data::Catalog& cat, MeleeModels& models, casc::Storage& store, const TestMap& map);

    void set_maps(std::vector<platform::MapEntry> maps);

    void set_maps_folder_callback(std::function<void()> cb) { maps_folder_cb_ = std::move(cb); }

    void set_texture_callback(std::function<void(formats::Model&, const formats::LoadedMap&)> cb) {
        texture_cb_ = std::move(cb);
    }

    bool step(const render::InputState& in, float dt_ms,
              std::vector<render::SceneInstance>& scene, render::Camera& cam);
    void build_overlay(std::uint32_t view_w, std::uint32_t view_h, std::vector<std::uint8_t>& out);

    const data::EnvLight& current_environment() const { return current_env_; }

    void enter_sandbox() { start_sandbox(); }
    void enter_skirmish() { start_skirmish(); }
    void enter_lobby() { state_ = State::Lobby; }

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
    bool showing_menu() const { return state_ == State::Menu; }
    bool showing_lobby() const { return state_ == State::Lobby; }
    bool showing_sandbox() const { return state_ == State::Sandbox; }

private:
    enum class State { Menu, Lobby, Skirmish, Sandbox };
    struct Button {
        int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
        std::string label;
        int action = 0;
        bool hit(float mx, float my) const {
            return mx >= float(x0) && mx < float(x1) && my >= float(y0) && my < float(y1);
        }
    };
    std::vector<Button> menu_buttons(std::uint32_t view_w, std::uint32_t view_h);
    std::vector<Button> lobby_buttons(std::uint32_t view_w, std::uint32_t view_h) const;

    const MainMenu* real_menu(std::uint32_t view_w, std::uint32_t view_h);
    void start_skirmish();
    void start_sandbox();
    void start_lobby_match();

    data::Catalog& cat_;
    MeleeModels& models_;
    casc::Storage& store_;
    const TestMap& map_;

    State state_ = State::Menu;
    std::unique_ptr<MeleeGame> skirmish_;
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

    std::vector<platform::MapEntry> maps_;
    int sel_map_ = 0;
    Race p0_race_ = Race::Terran;
    Race p1_race_ = Race::Terran;

    std::unique_ptr<formats::LoadedMap> real_map_;
    data::EnvLight current_env_;
    formats::Model real_ground_;
    std::vector<formats::PlacedDoodad> real_doodads_;
    CliffField real_cliffs_;

    std::function<void()> maps_folder_cb_;
    std::function<void(formats::Model&, const formats::LoadedMap&)> texture_cb_;

    render::OverlayFont overlay_font_;
    bool overlay_font_tried_ = false;
    void ensure_overlay_font();
};

}
