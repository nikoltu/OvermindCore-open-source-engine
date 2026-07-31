#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "sc2/app/melee_game.hpp"
#include "sc2/app/sandbox_game.hpp"
#include "sc2/app/test_map.hpp"
#include "sc2/render/camera_control.hpp"
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

    bool step(const render::InputState& in, float dt_ms,
              std::vector<render::SceneInstance>& scene, render::Camera& cam);
    void build_overlay(std::uint32_t view_w, std::uint32_t view_h, std::vector<std::uint8_t>& out);

    void enter_sandbox() { start_sandbox(); }
    void enter_skirmish() { start_skirmish(); }

private:
    enum class State { Menu, Skirmish, Sandbox };
    struct Button {
        int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
        std::string label;
        State target = State::Menu;
        bool hit(float mx, float my) const {
            return mx >= float(x0) && mx < float(x1) && my >= float(y0) && my < float(y1);
        }
    };
    std::vector<Button> menu_buttons(std::uint32_t view_w, std::uint32_t view_h) const;
    void start_skirmish();
    void start_sandbox();

    data::Catalog& cat_;
    MeleeModels& models_;
    casc::Storage& store_;
    const TestMap& map_;

    State state_ = State::Menu;
    std::unique_ptr<MeleeGame> skirmish_;
    std::unique_ptr<SandboxGame> sandbox_;
    render::CameraControl menu_ctl_;
    bool prev_left_ = false;
};

}
