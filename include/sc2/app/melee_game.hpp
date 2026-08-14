#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include "sc2/ai/melee_ai.hpp"
#include "sc2/app/ability.hpp"
#include "sc2/app/ability_card.hpp"
#include "sc2/app/alerts.hpp"
#include "sc2/app/command_bind.hpp"
#include "sc2/app/game_clock.hpp"
#include "sc2/app/control_groups.hpp"
#include "sc2/app/match_result.hpp"
#include "sc2/app/melee.hpp"
#include "sc2/app/melee_models.hpp"
#include "sc2/app/cast_policy.hpp"
#include "sc2/app/melee_research.hpp"
#include "sc2/app/selection.hpp"
#include "sc2/app/ui_icons.hpp"
#include "sc2/render/actor.hpp"
#include "sc2/render/camera_control.hpp"
#include "sc2/render/decals.hpp"
#include "sc2/render/fog.hpp"
#include "sc2/render/input.hpp"
#include "sc2/render/scene.hpp"
#include "sc2/render/unit_overlay.hpp"
#include "sc2/sim/mule.hpp"
#include "sc2/sim/nav.hpp"
#include "sc2/sim/scan_sweep.hpp"
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

    void set_spawn_models(int player, const formats::Model* worker, const formats::Model* army,
                          const formats::Model* producer);

    void set_build_verb(sim::BuildVerb verb) { human_build_verb_ = verb; }

    void enable_research(const data::Catalog& cat, const data::UpgradeCatalog& up,
                         std::vector<MeleeResearchDriver::ProtoNames> proto_names);
    void bind_catalog(const data::Catalog& cat) { cat_ = &cat; }
    void begin_research(int player, const std::string& id, sim::Fixed delay);

    struct AiResearchPlan {
        std::vector<std::string> upgrades;
        std::string tech_id;
    };
    void enable_ai_research(std::vector<AiResearchPlan> plans) { ai_research_ = std::move(plans); }
    const std::vector<std::string>& researched(int player) const;
    int research_in_progress() const;

    void enable_larva(const data::Catalog& cat);

    void register_creep_masks(const data::Catalog& cat);

    void enable_collapsible_debris(const data::Catalog& cat);

    void enable_ai_casting(const data::Catalog& cat,
                           const std::vector<std::pair<std::string, std::string>>& regs);

    int enable_ai_casting_from_roster(const data::Catalog& cat);

    bool human_cast(const std::string& abil_id, std::uint32_t target_id);
    bool human_cast_point(const std::string& abil_id, sim::Fixed px, sim::Fixed py);

    std::vector<AbilityCardEntry> selection_ability_entries() const;

    std::vector<AbilityCardEntry> selection_card_abilities();

    void begin_ability_cast(const std::string& abil_id, const std::string& command) {
        pending_cast_abil_ = abil_id;
        pending_cast_cmd_ = command;
    }
    bool has_pending_ability_cast() const { return !pending_cast_abil_.empty(); }
    const std::string& pending_ability_cast() const { return pending_cast_abil_; }
    void cancel_ability_cast() { pending_cast_abil_.clear(); pending_cast_cmd_.clear(); }
    bool cast_pending_at_target(std::uint32_t target_id) {
        if (pending_cast_abil_.empty()) return false;
        const bool ok = human_cast(pending_cast_abil_, target_id);
        if (ok) cancel_ability_cast();
        return ok;
    }
    bool cast_pending_at_point(sim::Fixed px, sim::Fixed py) {
        if (pending_cast_abil_.empty()) return false;
        const bool ok = human_cast_point(pending_cast_abil_, px, py);
        if (ok) cancel_ability_cast();
        return ok;
    }

    MatchResult::Outcome outcome() const { return outcome_; }

    void build_overlay(std::uint32_t view_w, std::uint32_t view_h, std::vector<std::uint8_t>& out);

    bool step(const render::InputState& in, float dt_ms,
              std::vector<render::SceneInstance>& scene, render::Camera& cam);

    sim::World& world() { return world_; }
    const sim::World& world() const { return world_; }
    const sim::ScanSweep& scan_sweep() const { return world_.scan_sweep(); }

    void set_speed(GameClock::Speed s) { clock_.set_speed(s); }
    GameClock::Speed speed() const { return clock_.speed(); }
    void set_paused(bool p) { clock_.set_paused(p); }
    void toggle_pause() { clock_.toggle_pause(); }
    bool paused() const { return clock_.paused(); }
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

    MeleeCommandContext command_context();

private:
    std::string human_gas_structure() const;
    std::string human_town_hall() const;
    bool human_is_protoss() const;
    std::uint32_t selected_gateway() const;
    void update_camera(const render::InputState& in, float dt_s);
    void handle_commands(const render::InputState& in, float aspect);
    void emit_scene(std::vector<render::SceneInstance>& scene);
    void snapshot_positions(std::vector<render::Vec3>& out) const;
    void rebuild_subgroups();
    std::vector<std::uint32_t> subgroup_ordered_selection() const;
    std::uint32_t human_lead_caster() const;
    std::vector<AbilityCardEntry> merge_selection_abilities(CommandCard& card) const;
    std::uint32_t selected_addon_producer() const;
    std::uint32_t selected_techlab_producer() const;
    std::uint32_t selected_command_center() const;
    std::uint32_t selected_orbital() const;
    std::uint32_t selected_completed_engbay() const;
    std::uint32_t selected_starport() const;
    std::uint32_t selected_liberator() const;
    std::uint32_t selected_liberator_ag() const;
    std::vector<std::uint32_t> selected_merge_templar() const;
    std::vector<std::uint32_t> selected_burrowers(bool burrowed) const;
    std::uint32_t selected_queen() const;
    std::uint32_t selected_infestor() const;
    std::uint32_t selected_liftoff_structure(bool flying) const;
    std::uint32_t selected_depot(bool lowered) const;
    std::uint32_t selected_loaded_transport() const;

    void maybe_ai_research(int player);

    MeleeGameConfig cfg_;
    sim::World world_;
    render::ActorDriver driver_;
    render::CameraControl camctl_;
    std::vector<std::unique_ptr<ai::MeleeAI>> ais_;
    std::unique_ptr<MeleeResearchDriver> research_;
    std::vector<AiResearchPlan> ai_research_;
    std::unique_ptr<CastPolicy> cast_policy_;
    const data::Catalog* cat_ = nullptr;
    std::string pending_cast_abil_, pending_cast_cmd_;
    AbilTargetMode pending_cast_mode_ = AbilTargetMode::Unit;
    MeleeLayout layout_;
    const formats::Model* ground_ = nullptr;

    std::vector<uint32_t> selection_;
    bool dragging_ = false;
    float drag_x0_ = 0, drag_y0_ = 0;
    float mouse_x_ = 0, mouse_y_ = 0;
    bool framed_ = false;
    float accum_ms_ = 0;
    GameClock clock_;
    int tick_counter_ = 0;

    std::vector<render::Vec3> prev_pos_;
    std::vector<render::Vec3> curr_pos_;

    const formats::Model* producer_model_ = nullptr;
    const formats::Model* carry_model_ = nullptr;
    const MeleeModels* models_ = nullptr;
    std::vector<const formats::Model*> spawn_worker_model_;
    std::vector<const formats::Model*> spawn_army_model_;
    std::vector<const formats::Model*> spawn_producer_model_;
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
    struct EdgeKeys {
        bool train = false, build = false, hold = false, stop = false, patrol = false;
    };
    EdgeKeys prev_keys_;
    bool ui_build_ = false;
    bool ui_train_ = false;
    std::uint32_t human_producer_ = sim::kNoTarget;
    bool human_producer_pending_ = false;
    struct HumanBuildConfig {
        sim::Unit worker_proto;
        sim::Unit supply_proto;
        sim::Unit producer_proto;
        int worker_cost = 50;
        int supply_cost = 100;
        int producer_cost = 150;
        int army_cost = 50;
        sim::Fixed worker_build_time = sim::Fixed::from_int(12);
        sim::Fixed supply_build_time = sim::Fixed::from_int(21);
        sim::Fixed producer_build_time = sim::Fixed::from_int(46);
    };
    HumanBuildConfig human_build_;
    bool ui_trainworker_ = false;
    bool ui_buildsupply_ = false;
    bool ui_buildtechlab_ = false;
    bool ui_buildreactor_ = false;
    bool ui_traintech_ = false;
    bool ui_buildfactory_ = false;
    bool ui_buildstarport_ = false;
    bool ui_trainair_ = false;
    bool ui_buildpylon_ = false;
    bool ui_buildgateway_ = false;
    bool ui_trainzealot_ = false;
    bool ui_morphorbital_ = false;
    bool ui_buildengbay_ = false;
    bool ui_research_ = false;
    std::int32_t orbital_proto_ = -1;
    std::int32_t liberator_ag_proto_ = -1;
    std::int32_t liberator_proto_ = -1;
    bool ui_fightermode_ = false;
    std::int32_t archon_proto_ = -1;
    bool ui_mergearchon_ = false;
    bool ui_burrow_ = false;
    bool ui_unburrow_ = false;
    bool ui_liftoff_ = false;
    bool ui_lowerdepot_ = false;
    bool ui_raisedepot_ = false;
    sim::MuleSystem mule_system_;
    bool pending_attack_ = false;
    bool pending_patrol_ = false;
    bool pending_mule_ = false;
    std::uint32_t pending_orbital_ = sim::kNoTarget;
    bool pending_defender_ = false;
    std::uint32_t pending_liberator_ = sim::kNoTarget;
    bool pending_land_ = false;
    std::uint32_t pending_land_unit_ = sim::kNoTarget;
    bool pending_unload_ = false;
    std::uint32_t pending_unload_unit_ = sim::kNoTarget;
    bool pending_inject_ = false;
    std::uint32_t pending_queen_ = sim::kNoTarget;
    bool pending_infested_ = false;
    std::uint32_t pending_infestor_ = sim::kNoTarget;
    bool pending_build_ = false;
    bool pending_refinery_ = false;
    bool pending_town_hall_ = false;
    sim::Unit build_proto_;
    int build_cost_ = 0;
    sim::Fixed build_time_;
    bool build_valid_ = false;
    sim::Fixed build_px_, build_py_;
    struct QueuedBuild {
        std::uint32_t worker;
        sim::Fixed x, y;
        sim::Unit proto;
        int cost;
        sim::Fixed time;
        bool is_producer;
    };
    std::vector<QueuedBuild> build_queue_;
    sim::BuildVerb human_build_verb_ = sim::BuildVerb::Committed;
    bool prev_num_[10] = {};

};

}
