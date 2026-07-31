#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include "sc2/ai/melee_ai.hpp"
#include "sc2/app/alerts.hpp"
#include "sc2/app/command_bind.hpp"
#include "sc2/app/control_groups.hpp"
#include "sc2/app/match_result.hpp"
#include "sc2/app/melee.hpp"
#include "sc2/app/melee_models.hpp"
#include "sc2/app/selection.hpp"
#include "sc2/app/ui_icons.hpp"
#include "sc2/render/actor.hpp"
#include "sc2/render/camera_control.hpp"
#include "sc2/render/decals.hpp"
#include "sc2/render/fog.hpp"
#include "sc2/render/input.hpp"
#include "sc2/render/scene.hpp"
#include "sc2/render/unit_overlay.hpp"
#include "sc2/sim/nav.hpp"
#include "sc2/sim/score.hpp"
#include "sc2/sim/vision.hpp"
#include "sc2/sim/world.hpp"

namespace sc2::formats {
struct Model;
}

namespace sc2::app {

struct MeleeGameConfig {
    int human_player = -1;
    int ai_period_ticks = 8;
    bool no_victory = false;
    float pan_speed = 18.0f;
    float edge_margin = 6.0f;
    float orbit_speed = 1.6f;
    float orbit_mouse = 0.005f;
    float zoom_step = 2.5f;
};

class MeleeGame {
public:
    explicit MeleeGame(MeleeGameConfig cfg = {});

    void setup(const MeleeSetup& setup, const MeleeModels* models,
               const formats::Model* ground, float map_w, float map_h,
               const sim::NavGrid* nav = nullptr);

    void set_ui_storage(casc::Storage* store) { ui_store_ = store; }

    MatchResult::Outcome outcome() const { return outcome_; }

    void build_overlay(std::uint32_t view_w, std::uint32_t view_h, std::vector<std::uint8_t>& out);

    bool step(const render::InputState& in, float dt_ms,
              std::vector<render::SceneInstance>& scene, render::Camera& cam);

    sim::World& world() { return world_; }
    const sim::World& world() const { return world_; }
    const MeleeLayout& layout() const { return layout_; }
    const std::vector<uint32_t>& selection() const { return selection_; }
    void set_selection(std::vector<uint32_t> sel) { selection_ = std::move(sel); }
    const MessageLog& messages() const { return messages_; }
    const sim::ScoreKeeper& score() const { return score_; }
    const SelectionGroups& subgroups() const { return sel_groups_; }
    render::CameraControl& camera_control() { return camctl_; }
    const render::ActorDriver& driver() const { return driver_; }
    float map_w() const { return map_w_; }
    float map_h() const { return map_h_; }

    std::uint32_t spawn(sim::Unit proto, const std::string& token,
                        const formats::Model* model, sim::Fixed x, sim::Fixed y);

private:
    void update_camera(const render::InputState& in, float dt_s);
    void handle_commands(const render::InputState& in, float aspect);
    void emit_scene(std::vector<render::SceneInstance>& scene);
    void snapshot_positions(std::vector<render::Vec3>& out) const;
    void rebuild_subgroups();
    std::vector<std::uint32_t> subgroup_ordered_selection() const;

    MeleeGameConfig cfg_;
    sim::World world_;
    render::ActorDriver driver_;
    render::CameraControl camctl_;
    std::vector<std::unique_ptr<ai::MeleeAI>> ais_;
    MeleeLayout layout_;
    const formats::Model* ground_ = nullptr;

    std::vector<uint32_t> selection_;
    bool dragging_ = false;
    float drag_x0_ = 0, drag_y0_ = 0;
    float mouse_x_ = 0, mouse_y_ = 0;
    bool framed_ = false;
    float accum_ms_ = 0;
    int tick_counter_ = 0;

    std::vector<render::Vec3> prev_pos_;
    std::vector<render::Vec3> curr_pos_;

    const formats::Model* producer_model_ = nullptr;
    const formats::Model* carry_model_ = nullptr;
    casc::Storage* ui_store_ = nullptr;
    IconAtlas icons_;
    std::size_t bound_count_ = 0;

    float map_w_ = 0, map_h_ = 0;
    float fps_smooth_ = 0.0f;
    sim::VisionGrid vision_;
    render::FogOverlay fog_;
    render::UnitOverlay unit_overlay_;
    ControlGroups groups_;
    MessageLog messages_;
    AlertWatcher alerts_;
    sim::ScoreKeeper score_;
    render::DecalField decals_;
    std::vector<std::uint8_t> death_seen_;
    SelectionGroups sel_groups_;
    MatchResult::Outcome outcome_ = MatchResult::Outcome::Ongoing;
    bool prev_train_ = false;
    bool prev_build_ = false;
    bool ui_build_ = false;
    bool ui_train_ = false;
    bool prev_hold_ = false;
    bool prev_stop_ = false;
    bool prev_patrol_ = false;
    std::uint32_t human_producer_ = sim::kNoTarget;
    bool human_producer_pending_ = false;
    int human_producer_cost_ = 150;
    int human_army_cost_ = 50;
    sim::Unit human_worker_proto_;
    int human_worker_cost_ = 50;
    sim::Fixed human_worker_build_time_ = sim::Fixed::from_int(12);
    sim::Unit human_supply_proto_;
    int human_supply_cost_ = 100;
    sim::Fixed human_supply_build_time_ = sim::Fixed::from_int(21);
    bool ui_trainworker_ = false;
    bool ui_buildsupply_ = false;
    bool pending_attack_ = false;
    bool pending_patrol_ = false;
    bool prev_num_[10] = {};

    MeleeCommandContext command_context();
};

}
