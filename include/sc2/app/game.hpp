#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

#include "sc2/ai/melee_ai.hpp"
#include "sc2/render/actor.hpp"
#include "sc2/render/camera_control.hpp"
#include "sc2/render/input.hpp"
#include "sc2/render/scene.hpp"
#include "sc2/sim/world.hpp"

namespace sc2::formats {
struct Model;
}

namespace sc2::app {

struct DemoLayout {
    std::vector<uint32_t> marines;
    std::vector<uint32_t> zerglings;
    float model_scale = 1.0f;
};

DemoLayout build_demo(sim::World& w, int human_player, int ai_player,
                      int n_marines = 5, int n_zerglings = 6);

struct SkirmishConfig {
    const formats::Model* marine = nullptr;
    const formats::Model* zergling = nullptr;
    const formats::Model* ground = nullptr;
    float model_size = 1.1f;
    int human_player = 0;
    int ai_player = 1;
    bool enable_ai = true;
    int n_marines = 5, n_zerglings = 6;

    float pan_speed = 18.0f;
    float edge_margin = 6.0f;
    float orbit_speed = 1.6f;
    float orbit_mouse = 0.005f;
    float zoom_step = 2.5f;

    int ai_period_ticks = 8;
};

class Skirmish {
public:
    explicit Skirmish(SkirmishConfig cfg);

    void setup_demo();

    bool step(const render::InputState& in, float dt_ms,
              std::vector<render::SceneInstance>& scene, render::Camera& cam);

    sim::World& world() { return world_; }
    const sim::World& world() const { return world_; }
    const std::vector<uint32_t>& selection() const { return selection_; }
    render::CameraControl& camera_control() { return camctl_; }
    const DemoLayout& layout() const { return layout_; }

private:
    void update_camera(const render::InputState& in, float dt_s);
    void handle_commands(const render::InputState& in, float aspect);
    void emit_scene(std::vector<render::SceneInstance>& scene);

    SkirmishConfig cfg_;
    sim::World world_;
    render::ActorDriver driver_;
    render::CameraControl camctl_;
    std::unique_ptr<ai::MeleeAI> ai_;
    DemoLayout layout_;

    std::vector<uint32_t> selection_;
    bool dragging_ = false;
    float drag_x0_ = 0, drag_y0_ = 0;
    bool framed_ = false;

    float accum_ms_ = 0;
    int tick_counter_ = 0;
};

}
