#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "sc2/data/sound_events.hpp"
#include "sc2/render/audio_sink.hpp"

#include "sc2/ai/melee_ai.hpp"
#include "sc2/app/ability.hpp"
#include "sc2/app/ability_card.hpp"
#include "sc2/app/alerts.hpp"
#include "sc2/app/command_bind.hpp"
#include "sc2/app/console_panel.hpp"
#include "sc2/app/hud_layout.hpp"
#include "sc2/app/game_clock.hpp"
#include "sc2/app/control_groups.hpp"
#include "sc2/app/match_result.hpp"
#include "sc2/app/melee.hpp"
#include "sc2/app/melee_models.hpp"
#include "sc2/app/replay.hpp"
#include "sc2/app/cast_policy.hpp"
#include "sc2/app/melee_research.hpp"
#include "sc2/app/selection.hpp"
#include "sc2/app/tooltip.hpp"
#include "sc2/app/ui_icons.hpp"
#include "sc2/data/production.hpp"
#include "sc2/render/actor.hpp"
#include "sc2/render/camera_control.hpp"
#include "sc2/render/decals.hpp"
#include "sc2/render/fog.hpp"
#include "sc2/render/font_ttf.hpp"
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
struct PlacedDoodad;
}

namespace sc2::data {
class Catalog;
class ActorCatalog;
class AudioCatalog;
}
namespace sc2::casc {
class Storage;
}
namespace sc2::audio {
class Mixer;
class WasapiSink;
}

namespace sc2::app {

struct ConsolePanel;

class MeleeAudio {
public:
    struct PcmClip {
        std::vector<std::int16_t> samples;
        std::uint32_t rate = 0;
        int channels = 0;
        bool empty() const { return samples.empty() || channels < 1; }
    };

    MeleeAudio();
    ~MeleeAudio();
    MeleeAudio(const MeleeAudio&) = delete;
    MeleeAudio& operator=(const MeleeAudio&) = delete;

    void bind_catalog(const data::Catalog& cat, casc::Storage* store);
    bool bound() const;

    bool start_device();
    bool device_active() const;

    int play_event(const std::string& unit_catalog_id, data::UnitEvent ev, float gain = 1.0f);

    int play_clip(const std::string& key, const PcmClip& clip, float gain = 1.0f);

    void set_category_gain(const std::string& category, float gain);
    float category_gain(const std::string& category) const;

    static float volume_db_pair_to_gain(const std::string& db_pair);

    static std::size_t select_variation(const std::string& sound_id, const std::string& select,
                                        std::size_t asset_count, std::uint64_t play_index);

    void render(std::int16_t* out, std::size_t frames);

    std::size_t cached_assets() const;
    std::size_t resolved_cue_count() const;
    std::size_t voice_count() const;

private:
    const PcmClip* decode_cached(const std::string& path);

    mutable std::mutex mtx_;
    std::unique_ptr<audio::Mixer> mixer_;
    std::unique_ptr<audio::WasapiSink> sink_;
    std::unordered_map<std::string, PcmClip> cache_;
    std::unique_ptr<data::ActorCatalog> actors_;
    std::unique_ptr<data::AudioCatalog> audio_cat_;
    casc::Storage* store_ = nullptr;
    std::unordered_map<std::string, char> resolved_;
    std::vector<int> recent_ids_;
    int max_active_voices_ = 24;
    std::unordered_map<std::string, std::uint64_t> play_count_;
    std::unordered_map<std::string, float> category_gain_;
};

struct MeleeGameConfig {
    int human_player = -1;
    int ai_period_ticks = 8;
    bool no_victory = false;
    float pan_speed = 68.5f;
    float edge_margin = 6.0f;
    float orbit_speed = 1.6f;
    float orbit_mouse = 0.005f;
    float zoom_step = 2.5f;
};

class MeleeGame {
public:
    explicit MeleeGame(MeleeGameConfig cfg = {});

    void setup(const MeleeSetup& setup, MeleeModels* models,
               const formats::Model* ground, float map_w, float map_h,
               const sim::NavGrid* nav = nullptr);

    void set_ui_storage(casc::Storage* store) { ui_store_ = store; }

    void set_doodads(const std::vector<formats::PlacedDoodad>* d) { doodads_ = d; }
    void set_cliffs(const std::vector<render::SceneInstance>* c) { cliffs_ = c; }

    void set_spawn_models(int player, const formats::Model* worker, const formats::Model* army,
                          const formats::Model* producer);

    void set_build_verb(sim::BuildVerb verb) { human_build_verb_ = verb; }

    void enable_research(const data::Catalog& cat, const data::UpgradeCatalog& up,
                         std::vector<MeleeResearchDriver::ProtoNames> proto_names);
    void bind_catalog(const data::Catalog& cat) {
        cat_ = &cat;
        production_.build(cat);
    }
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

    void enable_broodlings(const data::Catalog& cat);

    void enable_interceptors(const data::Catalog& cat);

    void enable_novas(const data::Catalog& cat);

    void enable_ai_casting(const data::Catalog& cat,
                           const std::vector<std::pair<std::string, std::string>>& regs);

    int enable_ai_casting_from_roster(const data::Catalog& cat);

    bool toggle_autocast(std::uint32_t unit, const std::string& abil_id);
    bool autocast_state(std::uint32_t unit, const std::string& abil_id);

    bool human_cast(const std::string& abil_id, std::uint32_t target_id);
    bool human_cast_point(const std::string& abil_id, sim::Fixed px, sim::Fixed py);

    std::vector<AbilityCardEntry> selection_ability_entries() const;

    std::vector<AbilityCardEntry> selection_card_abilities();

    struct TrainCardEntry {
        int slot = -1;
        std::string icon;
        std::string name;
        std::uint32_t producer = sim::kNoTarget;
        std::string unit;
        int minerals = 0, gas = 0;
        sim::Fixed time;
        bool requirement_met = true;
        bool tech_met = true;
    };
    bool human_train(std::uint32_t producer, const std::string& unit_id);
    std::vector<TrainCardEntry> selection_train_card();

    struct BuildCardEntry {
        int slot = -1;
        std::string icon;
        std::string name;
        std::uint32_t worker = sim::kNoTarget;
        std::string unit;
        int minerals = 0, gas = 0;
        sim::Fixed time;
        bool is_gas = false;
        bool requirement_met = true;
        bool tech_met = true;
    };
    bool human_build(std::uint32_t worker, const std::string& structure_unit_id);
    std::vector<BuildCardEntry> selection_build_card();

    bool human_morph(std::uint32_t caster, const std::string& morph_abil);
    bool human_merge();
    bool human_research();
    bool human_siege_defender(std::uint32_t caster, sim::Fixed cx, sim::Fixed cy);

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
    bool is_nova_cast(std::uint32_t caster, const std::string& abil) const {
        if (nova_ability_by_unit_.empty() || caster >= world_.units().size()) return false;
        auto it = nova_ability_by_unit_.find(world_.units()[caster].catalog_id);
        return it != nova_ability_by_unit_.end() && it->second == abil;
    }

    OrderLog current_replay_log() const {
        OrderLog snap = replay_log_;
        snap.total_ticks = world_.tick();
        return snap;
    }
    void set_replay_meta(std::string map_id, std::vector<std::int32_t> races) {
        replay_log_.map_id = std::move(map_id);
        replay_log_.player_races = std::move(races);
    }
    bool write_replay(const std::string& path) const {
        return write_order_log_file(current_replay_log(), path);
    }

    void begin_replay(const OrderLog& log);
    bool replay_active() const { return replay_drive_; }
    int replay_total_ticks() const { return replay_total_; }
    ReplayTrace run_replay_to_end();

    MatchResult::Outcome outcome() const { return outcome_; }

    void build_overlay(std::uint32_t view_w, std::uint32_t view_h, std::vector<std::uint8_t>& out);

    std::optional<TooltipInfo> hovered_card_tooltip(std::uint32_t view_w, std::uint32_t view_h,
                                                    float mx, float my,
                                                    const ConsolePanel* console = nullptr);

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
    bool pause_menu_open() const { return pause_menu_open_; }
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

    void emit_scene(std::vector<render::SceneInstance>& scene);

private:
    std::string human_gas_structure() const;
    std::string human_town_hall() const;
    bool human_is_protoss() const;
    int human_race() const;
    void ensure_minimap_terrain();
    void update_minimap_fog(int viewed);
    void update_camera(const render::InputState& in, float dt_s);
    void handle_commands(const render::InputState& in, float aspect);
    bool cancel_pending_modal();
    struct PauseRect { int x0, y0, x1, y1; };
    void pause_menu_rects(std::uint32_t view_w, std::uint32_t view_h, PauseRect& resume,
                          PauseRect& quit) const;
    void snapshot_positions(std::vector<render::Vec3>& out) const;
    void rebuild_subgroups();
    std::vector<std::uint32_t> subgroup_ordered_selection() const;
    std::uint32_t human_lead_caster() const;
    data::TechState human_tech_state() const;
    CommandCard compose_command_card(std::vector<BuildCardEntry>& builds,
                                     std::vector<AbilityCardEntry>& abils,
                                     std::vector<TrainCardEntry>& trains);
    std::vector<AbilityCardEntry> merge_selection_abilities(CommandCard& card) const;
    std::vector<TrainCardEntry> selection_train_entries() const;
    std::vector<TrainCardEntry> merge_selection_train(CommandCard& card) const;
    std::vector<BuildCardEntry> selection_build_entries() const;
    std::vector<BuildCardEntry> merge_selection_build(CommandCard& card) const;
    std::uint32_t selected_addon_producer() const;
    std::uint32_t selected_command_center() const;
    std::uint32_t selected_completed_engbay() const;
    std::uint32_t selected_constructing_building() const;
    std::uint32_t selected_liberator() const;
    std::uint32_t selected_liberator_ag() const;
    std::vector<std::uint32_t> selected_merge_templar() const;
    std::vector<std::uint32_t> selected_burrowers(bool burrowed) const;
    std::uint32_t selected_liftoff_structure(bool flying) const;
    std::uint32_t selected_depot(bool lowered) const;
    std::uint32_t selected_loaded_transport() const;

    void maybe_ai_research(int player);

    MeleeGameConfig cfg_;
    sim::World world_;
    OrderLog replay_log_;
    std::unique_ptr<LogRecorder> replay_recorder_;
    bool replay_drive_ = false;
    OrderLog replay_source_;
    std::size_t replay_cursor_ = 0;
    int replay_total_ = 0;
    void inject_replay_orders(int tick);
    bool on_live_gas_node(sim::Fixed wx, sim::Fixed wy) const {
        for (const sim::Unit& n : world_.units()) {
            if (!n.alive || !n.is_gas || n.resource_left <= 0) continue;
            const sim::Fixed dx = n.x - wx, dy = n.y - wy;
            const sim::Fixed rr = n.radius.raw() > 0 ? n.radius : sim::Fixed::from_int(1);
            if (dx * dx + dy * dy <= rr * rr) return true;
        }
        return false;
    }

    render::ActorDriver driver_;
    render::CameraControl camctl_;
    int zoom_level_ = 0;
    std::vector<std::unique_ptr<ai::MeleeAI>> ais_;
    std::unique_ptr<MeleeResearchDriver> research_;
    std::vector<AiResearchPlan> ai_research_;
    std::unique_ptr<CastPolicy> cast_policy_;
    const data::Catalog* cat_ = nullptr;
    data::Production production_;
    std::string pending_cast_abil_, pending_cast_cmd_;
    AbilTargetMode pending_cast_mode_ = AbilTargetMode::Unit;
    std::unordered_map<std::string, std::string> nova_ability_by_unit_;
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
    MeleeModels* models_ = nullptr;
    std::vector<const formats::Model*> spawn_worker_model_;
    std::vector<const formats::Model*> spawn_army_model_;
    std::vector<const formats::Model*> spawn_producer_model_;
    casc::Storage* ui_store_ = nullptr;
    const std::vector<formats::PlacedDoodad>* doodads_ = nullptr;
    const std::vector<render::SceneInstance>* cliffs_ = nullptr;
    std::unique_ptr<MeleeAudio> audio_;
    render::AudioSink audio_sink_;
    void ensure_audio();
    std::vector<std::uint8_t> audio_known_;
    std::vector<std::uint8_t> audio_alive_;
    std::vector<sim::Fixed> audio_cd_;
    bool audio_seeded_ = false;
    void audio_observe_events(std::vector<std::uint32_t>& played);
    void audio_ui_event(std::uint32_t unit_id, data::UnitEvent ev, float gain);
    std::uint32_t audio_sel_lead_ = sim::kNoTarget;
    IconAtlas icons_;
    render::OverlayFont overlay_font_;
    bool overlay_font_tried_ = false;
    void ensure_overlay_font();
    ConsoleTexture console_tex_;
    const HudAuthoredRects* hud_authored_ = nullptr;
    bool console_tried_ = false;
    void ensure_console_texture();
    ConsolePanel console_view() const;
    void refresh_hud_layout(std::uint32_t view_w, std::uint32_t view_h);
    std::size_t bound_count_ = 0;
    std::vector<std::string> bound_cat_;

    float map_w_ = 0, map_h_ = 0;
    float fps_smooth_ = 0.0f;
    sim::VisionGrid vision_;
    render::FogOverlay fog_;
    std::vector<std::uint8_t> minimap_terrain_;
    int minimap_tw_ = 0, minimap_th_ = 0;
    bool minimap_terrain_baked_ = false;
    std::vector<std::uint8_t> minimap_fog_;
    int minimap_fw_ = 0, minimap_fh_ = 0;
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
    bool ui_buildtechlab_ = false;
    bool ui_buildreactor_ = false;
    bool pending_attack_ = false;
    bool pending_patrol_ = false;
    bool pending_defender_ = false;
    std::uint32_t pending_liberator_ = sim::kNoTarget;
    bool pending_land_ = false;
    std::uint32_t pending_land_unit_ = sim::kNoTarget;
    bool pending_unload_ = false;
    std::uint32_t pending_unload_unit_ = sim::kNoTarget;
    bool pending_build_ = false;
    bool pending_refinery_ = false;
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

    bool pause_menu_open_ = false;
    bool prev_esc_ = false;

};

}
