#pragma once

#include <cstdint>
#include <cstdlib>
#include <map>
#include <set>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "omc/data/sound_events.hpp"
#include "omc/formats/placed_objects.hpp"
#include "omc/render/audio_sink.hpp"

#include "omc/ai/difficulty.hpp"
#include "omc/ai/melee_ai.hpp"
#include "omc/app/hud.hpp"
#include "omc/app/ability.hpp"
#include "omc/app/overlay_draw.hpp"
#include "omc/app/ability_card.hpp"
#include "omc/app/alerts.hpp"
#include "omc/app/command_bind.hpp"
#include "omc/app/control.hpp"
#include "omc/app/camera_bookmarks.hpp"
#include "omc/app/camera_follow_data.hpp"
#include "omc/render/camera_follow.hpp"
#include "omc/render/fog_field.hpp"
#include "omc/render/texture_decode.hpp"
#include "omc/app/console_panel.hpp"
#include "omc/app/hud_layout.hpp"
#include "omc/app/hud_buttons.hpp"
#include "omc/app/hud_ui_state.hpp"
#include "omc/app/game_clock.hpp"
#include "omc/app/control_groups.hpp"
#include "omc/app/match_result.hpp"
#include "omc/app/melee.hpp"
#include "omc/app/melee_models.hpp"
#include "omc/app/replay.hpp"
#include "omc/app/cast_policy.hpp"
#include "omc/app/melee_research.hpp"
#include "omc/app/selection.hpp"
#include "omc/app/tooltip.hpp"
#include "omc/app/ui_icons.hpp"
#include "omc/ui/layout.hpp"
#include "omc/data/production.hpp"
#include "omc/data/actor_catalog.hpp"
#include "omc/render/actor.hpp"
#include "omc/render/actor_rules.hpp"
#include "omc/app/effect_actor_host.hpp"
#include "omc/render/camera_control.hpp"
#include "omc/render/cloak_look.hpp"
#include "omc/render/decals.hpp"
#include "omc/render/fog.hpp"
#include "omc/render/font_ttf.hpp"
#include "omc/render/input.hpp"
#include "omc/render/scene.hpp"
#include "omc/render/unit_overlay.hpp"
#include "omc/sim/mule.hpp"
#include "omc/sim/nav.hpp"
#include "omc/sim/scan_sweep.hpp"
#include "omc/sim/score.hpp"
#include "omc/sim/vision.hpp"
#include "omc/sim/world.hpp"

namespace omc::formats {
struct Model;
struct PlacedDoodad;
}

namespace omc::data {
class Catalog;
class ActorCatalog;
class AudioCatalog;
}
namespace omc::casc {
class Storage;
}
namespace omc::audio {
class Mixer;
class WasapiSink;
}

namespace omc::app {

sim::World::AddonBodyFactory addon_body_factory(const data::Catalog& cat);

int card_slot_for_key(const CommandCard& card, char key);

struct MinimapExtent;
struct ConsolePanel;
struct OrderCueState;

struct WireMask {
    int w = 0, h = 0;
    std::vector<std::uint8_t> cov;
};

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

    void bind_catalog(const data::Catalog& cat, const data::ActorCatalog& actors,
                      casc::Storage* store);
    bool bound() const;

    bool start_device();
    bool device_active() const;

    int play_event(const std::string& unit_catalog_id, data::UnitEvent ev, float gain = 1.0f,
                   float listener_distance = -1.0f, float listener_pan = 0.0f);

    int play_clip(const std::string& key, const PcmClip& clip, float gain = 1.0f, double rate_factor = 1.0);
    int play_clip_limited(const std::string& sound_id, int dupe_destroy, int dupe_mute, const std::string& key,
                          const PcmClip& clip, float gain = 1.0f, double rate_factor = 1.0, bool dupe_prevent = false,
                          int priority = 32);
    float last_rolloff_gain() const { return last_rolloff_gain_; }
    int play_sound(const std::string& sound_id, float gain = 1.0f);
    int last_voice_id() const { return last_voice_id_; }
    bool voice_active(int id) const;
    int last_voice_id_ = -1;
    const std::string& last_sound_id() const { return last_sound_id_; }
    std::string last_sound_id_;
    float last_rolloff_gain_ = 1.0f;

    void set_category_gain(const std::string& category, float gain);
    float category_gain(const std::string& category) const;

    enum class MixState { Game, Cinematic, Speech, Victory };
    void apply_mix_state(MixState s);
    MixState mix_state() const;

    static float volume_db_pair_to_gain(const std::string& db_pair);
    static float volume_db_pair_to_gain(const std::string& db_pair, const std::string& sound_id,
                                        std::uint64_t play_index);
    static std::uint64_t play_hash(const std::string& sound_id, std::uint64_t play_index);
    static constexpr float kDefaultAssetWeight = 10.0f;
    static std::size_t weighted_pick(const std::vector<float>& weights, std::uint64_t hash);
    static bool chance_allows(const std::optional<std::string>& chance, std::uint64_t hash);
    static double pitch_rate_factor(const std::string& pitch_pair, const std::string& sound_id,
                                    std::uint64_t play_index);

    static std::size_t select_variation(const std::string& sound_id, const std::string& select,
                                        std::size_t asset_count, std::uint64_t play_index);

    void render(std::int16_t* out, std::size_t frames);

    std::size_t cached_assets() const;
    std::size_t resolved_cue_count() const;
    std::size_t voice_count() const;
    std::uint32_t voice_src_rate(int id) const;

private:
    const PcmClip* decode_cached(const std::string& path);

    mutable std::mutex mtx_;
    std::unique_ptr<audio::Mixer> mixer_;
    std::unique_ptr<audio::WasapiSink> sink_;
    std::unordered_map<std::string, PcmClip> cache_;
    const data::ActorCatalog* actors_ = nullptr;
    std::unique_ptr<data::AudioCatalog> audio_cat_;
    casc::Storage* store_ = nullptr;
    std::unordered_map<std::string, char> resolved_;
    std::vector<int> recent_ids_;
    struct LiveVoice {
        int id;
        std::string sound;
        int priority;
    };
    std::vector<LiveVoice> voice_sounds_;
    int pending_priority_ = 32;
    float pending_pan_ = 0.0f;
    int max_active_voices_ = 24;
    std::unordered_map<std::string, std::uint64_t> play_count_;
    std::unordered_map<std::string, float> category_gain_;
    MixState mix_state_ = MixState::Game;
};

struct MeleeGameConfig {
    int human_player = -1;
    std::uint64_t seed = 1;
    int ai_period_ticks = 8;
    int ai_difficulty = 0;
    std::vector<int> slot_difficulty;
    bool no_victory = false;
    float pan_speed = 68.5f;
    float edge_margin = 6.0f;
    float orbit_speed = 1.6f;
    float orbit_mouse = 0.005f;
    float pan_mouse = 0.05f;
    float zoom_step = 2.5f;
};

render::ActorDriver::BurrowPair depot_morph_pair(const data::Catalog& cat, const std::string& unit_id);

std::array<float, 4> team_colour_for(const data::Catalog* cat, int player, bool selected);
std::array<float, 4> team_emissive_for(const data::Catalog* cat, int player);
void set_map_team_slots(const std::array<int, 16>& slots);
std::array<std::uint8_t, 3> minimap_attacked_rgb(const data::Catalog* cat, int player);

class MeleeGame {
public:
    explicit MeleeGame(MeleeGameConfig cfg = {});
    ~MeleeGame() {
        if (render::TextureDecodePool* pool = render::TextureDecodePool::existing()) pool->drain();
        if (draw::active_font() == &overlay_font_) draw::set_active_font(nullptr);
        for (const auto& kv : style_faces_)
            if (draw::active_font() == &kv.second) draw::set_active_font(nullptr);
    }

    void setup(const MeleeSetup& setup, MeleeModels* models,
               const formats::Model* ground, float map_w, float map_h,
               const sim::NavGrid* nav = nullptr,
               const sim::TerrainMap* placement = nullptr, float playable_x0 = -1.0f,
               float playable_y0 = -1.0f, float playable_x1 = -1.0f, float playable_y1 = -1.0f);

    void set_ui_storage(casc::Storage* store) { ui_store_ = store; }

    void set_doodads(const std::vector<formats::PlacedDoodad>* d) { doodads_ = d; }
    void set_fluff(const std::vector<formats::PlacedDoodad>* d) { fluff_ = d; }
    void set_camera_bounds_x(float left, float right) { cam_left_ = left; cam_right_ = right; }
    void set_cliffs(const std::vector<render::SceneInstance>* c) { cliffs_ = c; }

    void set_tier_heights(std::vector<float> th) { tier_heights_ = std::move(th); }
    void set_air_floor(float z) {
        air_floor_ = z;
        rebuild_air_layer();
    }
    float air_z_at(float wx, float wy) const;
    void set_water(const formats::Model* water, const std::array<float, 4>& rgba) {
        water_ = water;
        water_rgba_ = rgba;
    }
    void set_map_water_xml(const std::vector<std::uint8_t>& xml) {
        map_water_xml_ = xml;
        water_look_ = WaterLook{};
    }
    void set_roads(const formats::Model* roads) { roads_ = roads; }
    void set_minimap_image(const std::vector<std::uint8_t>& rgba, std::uint32_t w, std::uint32_t h) {
        if (!w || !h || rgba.size() != std::size_t(w) * h * 4) return;
        minimap_terrain_.assign(std::size_t(w) * h * 3, 0);
        for (std::size_t y = 0; y < h; ++y)
            for (std::size_t x = 0; x < w; ++x) {
                const std::size_t si = ((std::size_t(h) - 1 - y) * w + x) * 4, di = (y * w + x) * 3;
                minimap_terrain_[di + 0] = rgba[si + 0];
                minimap_terrain_[di + 1] = rgba[si + 1];
                minimap_terrain_[di + 2] = rgba[si + 2];
            }
        minimap_terrain_authored_ = true;
        minimap_tw_ = int(w);
        minimap_th_ = int(h);
        minimap_terrain_baked_ = true;
    }

    void set_vertex_heights(std::vector<float> grid, std::uint32_t w, std::uint32_t h) {
        vertex_heights_ = std::move(grid);
        vertex_w_ = w;
        vertex_h_ = h;
    }
    float vertex_ground_at(float wx, float wy) const {
        if (vertex_heights_.empty() || vertex_w_ < 2 || vertex_h_ < 2) return ground_z_at(wx, wy);
        const float x = std::min(std::max(wx, 0.0f), float(vertex_w_ - 1)), y = std::min(std::max(wy, 0.0f), float(vertex_h_ - 1));
        const std::uint32_t c = std::min(std::uint32_t(x), vertex_w_ - 2), r = std::min(std::uint32_t(y), vertex_h_ - 2);
        const float fx = x - float(c), fy = y - float(r);
        const auto z = [&](std::uint32_t cc, std::uint32_t rr) { return vertex_heights_[std::size_t(rr) * vertex_w_ + cc]; };
        return (z(c, r) * (1.0f - fx) + z(c + 1, r) * fx) * (1.0f - fy) + (z(c, r + 1) * (1.0f - fx) + z(c + 1, r + 1) * fx) * fy;
    }
    void set_ground_heights(std::vector<float> grid, std::uint32_t w, std::uint32_t h) {
        ground_heights_ = std::move(grid);
        ground_w_ = w;
        ground_h_ = h;
        rebuild_air_layer();
    }
    static constexpr int kAirDilateCells = 2;
    static constexpr int kAirBlurRadius = 2;
    static constexpr int kAirBlurPasses = 2;
    void rebuild_air_layer();
    const std::vector<float>& air_layer() const { return air_layer_; }

    using ConsoleRenderer = std::function<ConsoleTexture(casc::Storage&, const data::Catalog&,
                                                         const std::string&, std::uint32_t, std::uint32_t)>;
    void set_console_renderer(ConsoleRenderer fn) { console_renderer_ = std::move(fn); }
    void refresh_console_texture() { console_tried_ = false; }
    std::string console_skin_id() const { return console_skin_id_for_race(human_race()); }
    bool console_ready() const { return console_tex_.ok(); }
    using PortraitRenderer = std::function<std::vector<std::uint8_t>(casc::Storage&, const formats::Model&, float fov_rad,
                                                                     int cam_bone, float znear, float zfar, std::uint32_t w, std::uint32_t h,
                                                                     const data::EnvLight* env)>;
    void set_portrait_renderer(PortraitRenderer fn) { portrait_renderer_ = std::move(fn); }
    void set_selection_warm(bool on) { selection_warm_ = on; }
    void set_portrait_hz(float hz) { portrait_hz_ = hz > 0.0f ? hz : 0.0f; }
    void set_portrait_async(bool on) { portrait_async_ = on; }
    void set_warm_foreign(bool on) { warm_foreign_ = on; }
    void set_card_cache(bool on) { card_cache_on_ = on; }
    void set_morph_transition_models(bool on) { morph_transition_models_ = on; }
    void set_morph_upgrade_fold(bool on) { morph_upgrade_fold_ = on; }
    void set_morph_lock_order(bool on) { morph_lock_order_ = on; }
    void set_morph_swap_at_data(bool on) { morph_swap_at_data_ = on; }
    void set_morph_start_delay(bool on) { morph_start_delay_ = on; }
    void set_morph_behaviors_sim(bool on) { morph_behaviors_sim_ = on; }
    void set_upgrade_fold_new(bool on) { upgrade_fold_new_ = on; }
    sim::Unit research_body(const std::string& unit_id, std::int32_t player) const;
    void set_timed_burrow(bool on) { timed_burrow_ = on; }
    void set_morph_cocoon_models(bool on) { morph_cocoon_models_ = on; }
    void set_zone_markers(bool on) { zone_markers_ = on; }
    void set_actor_game_clock(bool on) { actor_game_clock_ = on; }
    std::optional<data::Catalog::MorphSpec> player_morph_spec(std::int32_t player, const std::string& abil) const;
    bool human_burrow(std::uint32_t id, bool down);
    void set_timed_depot(bool on) { timed_depot_ = on; }
    bool human_depot(std::uint32_t id, bool down);
    void set_construction_child_team(bool on) { construction_child_team_ = on; }
    std::size_t card_cache_hits() const { return card_cache_hits_; }
    float portrait_hz() const { return portrait_hz_; }

    float ground_z_at(float wx, float wy) const;
    float formation_diameter_[2] = {6.0f, 8.0f};
    float air_floor_ = 0.0f;
    std::vector<float> air_layer_;
    static constexpr int kAirSmoothCells = 2;

    struct GroundZ : GroundHeight {
        explicit GroundZ(const MeleeGame* g) : game(g) {}
        const MeleeGame* game = nullptr;
        float z_at(float wx, float wy) const override { return game->ground_z_at(wx, wy); }
        float max_z() const override;
    };

    Control make_control(const render::Mat4& mvp, std::uint32_t view_w, std::uint32_t view_h,
                         std::int32_t player) const {
        Control c(mvp, view_w, view_h, player);
        c.set_ground_height(&ground_height_);
        if (!std::getenv("OMC_PICK_EXTENT_OLD")) c.set_pick_volumes(&pick_volumes_);
        if (select_rules_672_) {
            Control::BoxRules br;
            br.on = true;
            br.rank = [this](const sim::Unit& u) { return select_rank(u); };
            c.set_box_rules(std::move(br));
        }
        return c;
    }
    void set_select_rules_672(bool on) { select_rules_672_ = on; }
    int select_rank(const sim::Unit& u) const;

    struct PickPose {
        const formats::Model* model = nullptr;
        render::Mat4 world;
        int seq = -1;
        float time_ms = 0.0f;
        std::uint64_t frame = 0;
    };
    const std::vector<PickPose>& pick_poses() const { return pick_pose_; }
    std::uint32_t click_select_at(const Control& ctrl, const ScreenPoint& p) const;
    struct PickVolumesOf : PickVolumes {
        explicit PickVolumesOf(const MeleeGame* g) : game(g) {}
        const MeleeGame* game = nullptr;
        bool volumes(std::uint32_t id, std::vector<PickVolume>& out) const override;
        bool may_hit(std::uint32_t id, const float* o, const float* d) const override;
        bool broadphase = true;
        mutable std::map<std::pair<const formats::Model*, int>, float> reach;
    };
    void set_pick_broadphase(bool on) { pick_volumes_.broadphase = on; }
    const PickVolumes& pick_volumes_for_test() const { return pick_volumes_; }

    void set_spawn_models(int player, const formats::Model* worker, const formats::Model* army,
                          const formats::Model* producer);

    void set_build_verb(sim::BuildVerb verb) { human_build_verb_ = verb; }
    sim::BuildVerb build_verb_for(std::uint32_t builder) const;

    void enable_research(const data::Catalog& cat, const data::UpgradeCatalog& up,
                         std::vector<MeleeResearchDriver::ProtoNames> proto_names);
    void bind_catalog(const data::Catalog& cat) {
        cat_ = &cat;
        cloak_look_bound_ = 0;
        wire_addon_bodies();
        build_actor_catalog(cat);
        follow_.p = camera_follow_params(cat, melee_camera_profile(cat));
        production_.build(cat);
        AlertWatcher::Overlap ov;
        auto num = [&](const char* f, float& out) {
            if (auto v = cat.scalar_num("CAlert", "Attack", f)) out = float(*v);
        };
        num("OverlapDuration", ov.duration);
        num("OverlapLocalRadius", ov.local_radius);
        num("Life", ov.life);
        if (auto v = cat.scalar_num("CAlert", "Attack", "OverlapLocalCount")) ov.local_count = int(*v);
        if (auto v = cat.scalar_num("CAlert", "Attack", "OverlapGlobalCount")) ov.global_count = int(*v);
        alerts_.set_overlap(ov);
    }
    void begin_research(int player, const std::string& id, sim::Fixed delay, std::uint32_t producer = sim::kNoTarget,
                        int minerals = 0, int gas = 0);

    struct AiResearchPlan {
        std::vector<std::string> upgrades;
        std::string tech_id;
    };
    void enable_ai_research(std::vector<AiResearchPlan> plans) { ai_research_ = std::move(plans); }
    const std::vector<std::string>& researched(int player) const;
    int research_in_progress() const;

    void enable_larva(const data::Catalog& cat);

    void register_creep_masks(const data::Catalog& cat);
    void bind_actor_rules(const data::Catalog& cat);

    void enable_collapsible_debris(const data::Catalog& cat);

    void enable_broodlings(const data::Catalog& cat);

    void enable_interceptors(const data::Catalog& cat);

    void enable_novas(const data::Catalog& cat);

    void enable_ai_casting(const data::Catalog& cat,
                           const std::vector<std::pair<std::string, std::string>>& regs);

    int enable_ai_casting_from_roster(const data::Catalog& cat);
    void enable_ai_tech_gate(const data::Catalog& cat);
    void set_train_gate_strict(bool on) { train_gate_strict_ = on; }
    std::string train_ability_listed(std::uint32_t producer, const std::string& unit_id, bool at_site) const;
    void set_ai_stock_gas(bool on) {
        for (auto& a : ais_)
            if (a) a->set_stock_gas(on);
    }
    void set_ai_macro(bool pending_per_trainer, bool supply_ahead, bool tech_counts_sites, bool extra_producers, bool alt_fallback,
                      bool gas_orphans_mine) {
        for (auto& a : ais_)
            if (a) {
                a->set_gas_orphans_mine(gas_orphans_mine);
                a->set_alt_fallback(alt_fallback);
                a->set_pending_per_trainer(pending_per_trainer);
                a->set_supply_ahead(supply_ahead);
                a->set_tech_counts_sites(tech_counts_sites);
                a->set_extra_producers(extra_producers);
            }
    }
    void set_ai_close_out(bool multi_expand, bool larva_macro, bool hunt, bool respect_evict, bool gather_needs_dropoff) {
        for (auto& a : ais_)
            if (a) {
                a->set_multi_expand(multi_expand);
                a->set_larva_macro(larva_macro);
                a->set_hunt(hunt);
                a->set_respect_evict(respect_evict);
                a->set_gather_needs_dropoff(gather_needs_dropoff);
            }
        ai_inject_ = larva_macro;
    }
    void set_hunt_gas(bool on) {
        for (auto& a : ais_)
            if (a) a->set_hunt_gas(on);
    }
    void set_worker_transfer(bool on) {
        for (auto& a : ais_)
            if (a) a->set_worker_transfer(on);
    }
    int workers_transferred() const {
        int n = 0;
        for (const auto& a : ais_)
            if (a) n += a->workers_transferred();
        return n;
    }
    void set_hall_needs_builder(bool on) {
        hall_needs_builder_ = on;
        for (auto& a : ais_)
            if (a) a->set_hall_builder(on);
    }
    int ai_injects() const { return ai_injects_; }
    const ai::MeleeAI* ai_of(int p) const { return p >= 0 && std::size_t(p) < ais_.size() ? ais_[std::size_t(p)].get() : nullptr; }
    ai::MeleeAI* ai_of(int p) { return p >= 0 && std::size_t(p) < ais_.size() ? ais_[std::size_t(p)].get() : nullptr; }
    void pause_ai(bool on) { ai_paused_ = on; }
    bool ai_paused() const { return ai_paused_; }
    void wire_addon_bodies();

    bool toggle_autocast(std::uint32_t unit, const std::string& abil_id);
    bool autocast_state(std::uint32_t unit, const std::string& abil_id);

    bool human_cast(const std::string& abil_id, std::uint32_t target_id);
    int human_cast_instant(const std::string& abil_id);
    bool human_cast_by(const std::string& abil_id, std::uint32_t caster);
    void set_group_self_cast(bool on) { group_self_cast_ = on; }
    void set_group_cast_cell(bool on) { group_cast_cell_ = on; }
    void set_best_unit_pick(bool on) { best_unit_pick_ = on; }
    std::vector<std::pair<std::uint32_t, std::string>> group_cell_casters(const std::string& abil_id) const;
    bool human_cast_point(const std::string& abil_id, sim::Fixed px, sim::Fixed py);
    std::size_t effect_doodad_count() const { return effect_doodads_.size(); }
    const EffectActorHost& effect_actors() const { return fx_host_; }
    void set_placement_ghost(const std::string& unit_id, sim::Fixed x, sim::Fixed y, bool rules_ok);
    void clear_placement_ghost() { ghost_.active = false; }
    bool placement_ghost_active() const { return ghost_.active; }
    const auto& placement_ghost() const { return ghost_; }
    std::size_t placement_ghost_cells(std::size_t* green = nullptr) const {
        if (green) {
            *green = 0;
            for (char c : ghost_.cell_ok) *green += c ? 1u : 0u;
        }
        return ghost_.cells.size();
    }
    const formats::Model* placement_ghost_model() const;
    std::string card_unit_id(const sim::Unit& u) const;
    bool land_site(std::uint32_t id, sim::Fixed& x, sim::Fixed& y) const;
    struct PaneQueueSlot {
        std::string subject;
        float progress = 0.0f;
        std::string activity;
    };
    std::vector<PaneQueueSlot> pane_queue(std::uint32_t sel) const;
    bool morph_blocks_queue(std::uint32_t id) const;
    bool human_cancel_morph(const std::string& morph_abil);
    bool human_cancel_effect(const std::string& abil);
    bool effect_cancel_live(std::uint32_t id) const;
    bool morph_start_blocked(std::uint32_t id, const std::string& morph_abil) const;
    std::string effect_doodad_model_id(std::size_t i) const { return i < effect_doodads_.size() ? effect_doodads_[i].model_id : std::string(); }

    std::vector<AbilityCardEntry> selection_ability_entries() const;

    std::vector<AbilityCardEntry> selection_card_abilities();

    struct TrainCardEntry {
        int slot = -1;
        int card = 0;
        std::string icon;
        std::string name;
        std::string hotkey;
        std::uint32_t producer = sim::kNoTarget;
        std::string unit;
        int minerals = 0, gas = 0;
        sim::Fixed time;
        bool requirement_met = true;
        bool tech_met = true;
        bool warp = false;
        double warp_charge = 0;
    };
    bool human_train(std::uint32_t producer, const std::string& unit_id);
    bool human_warp_train(std::uint32_t producer, const std::string& unit_id, sim::Fixed tx,
                          sim::Fixed ty);
    std::vector<TrainCardEntry> selection_train_card();

    struct BuildCardEntry {
        int slot = -1;
        std::string icon;
        std::string name;
        std::string hotkey;
        std::uint32_t worker = sim::kNoTarget;
        std::string unit;
        int minerals = 0, gas = 0;
        sim::Fixed time;
        bool is_gas = false;
        bool requirement_met = true;
        bool tech_met = true;
        int card = 0;
    };
    bool human_build(std::uint32_t worker, const std::string& structure_unit_id);
    bool human_land_addon(std::uint32_t host, sim::Fixed x, sim::Fixed y, const std::string& addon);
    std::vector<BuildCardEntry> selection_build_card();

    std::vector<BuildCardEntry> selection_build_products() const { return selection_build_entries(); }

    int card_page() const { return card_page_; }
    void set_card_page(int page) { card_page_ = page < 0 ? 0 : page; }
    int card_page_of_submenu(const std::string& card_id) const;
    const std::vector<std::vector<std::string>>& construction_ladder(const std::string& catalog_id);
    std::unordered_map<std::string, std::vector<std::vector<std::string>>> construction_ladders_;
    bool lead_authors_card_page(int page) const;

    struct SubmenuCardEntry {
        int slot = -1;
        int target_page = 0;
        std::string icon;
        std::string hotkey;
        std::string name;
    };
    struct CardLayout {
        CommandCard card;
        std::vector<BuildCardEntry> builds;
        std::vector<AbilityCardEntry> abilities;
        std::vector<TrainCardEntry> trains;
        std::vector<SubmenuCardEntry> submenus;
    };
    CardLayout command_card_layout();
    std::vector<SubmenuCardEntry> selection_submenu_card();

    bool human_morph(std::uint32_t caster, const std::string& morph_abil);
    int human_morph_selection(const std::string& morph_abil);
    bool morph_command_allowed(std::uint32_t body, const std::string& morph_abil) const;
    int run_morph_autocast();
    std::string morph_ability_of(std::uint32_t id) const {
        const auto it = morph_abil_.find(id);
        return it != morph_abil_.end() && world_.is_morphing(id) ? it->second : std::string();
    }
    bool human_merge();

    bool human_toggle_cloak(const std::string& abil_id, const std::string& command);
    bool human_toggle_weapon(const std::string& abil_id, const std::string& command);
    bool human_toggle_creep(const std::string& abil_id, const std::string& command);

    bool human_salvage(const std::string& abil_id);
    bool human_research(const std::string& abil_id, const std::string& index, std::uint32_t producer);
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
    void set_replay_meta(std::string map_id, std::vector<std::int32_t> races, std::vector<std::int32_t> teams = {},
                         std::vector<std::int32_t> controllers = {}, bool any_start_pair = false) {
        replay_log_.any_start_pair = any_start_pair;
        replay_log_.map_id = std::move(map_id);
        replay_log_.player_races = std::move(races);
        replay_log_.player_teams = std::move(teams);
        replay_log_.player_controllers = std::move(controllers);
    }
    bool write_replay(const std::string& path) const {
        return write_order_log_file(current_replay_log(), path);
    }

    void begin_replay(const OrderLog& log);
    bool replay_active() const { return replay_drive_; }
    void resume_live() {
        if (!replay_drive_) return;
        replay_drive_ = false;
        replay_log_.orders = replay_source_.orders;
        if (replay_log_.map_id.empty()) replay_log_.map_id = replay_source_.map_id;
        if (replay_log_.player_races.empty()) replay_log_.player_races = replay_source_.player_races;
        world_.set_order_sink(replay_recorder_.get());
    }
    int replay_total_ticks() const { return replay_total_; }
    ReplayTrace run_replay_to_end();

    MatchResult::Outcome outcome() const { return outcome_; }
    void surrender(int player);
    int surrendered_player() const { return surrendered_player_; }
    bool viewer_defeated() const;
    void set_stalemate(sim::Fixed test_s, sim::Fixed warning_s) {
        stalemate_ticks_ = int(((test_s + warning_s) * sim::Fixed::from_int(sim::World::kTicksPerSecond)).round());
    }
    int stalemate_ticks() const { return stalemate_ticks_; }

    void build_overlay(std::uint32_t view_w, std::uint32_t view_h, std::vector<std::uint8_t>& out);

    std::optional<TooltipInfo> hovered_card_tooltip(std::uint32_t view_w, std::uint32_t view_h,
                                                    float mx, float my,
                                                    const ConsolePanel* console = nullptr);

    bool step(const render::InputState& in, float dt_ms,
              std::vector<render::SceneInstance>& scene, render::Camera& cam);

    sim::World& world() { return world_; }
    const sim::World& world() const { return world_; }
    struct Corpse {
        const formats::Model* model = nullptr;
        int seq = -1;
        float time_ms = 0.0f;
        float length_ms = 0.0f;
        float x = 0.0f, y = 0.0f, z = 0.0f;
        std::array<float, 3> scale = {1.0f, 1.0f, 1.0f};
        int player = -1;
        int fog_visibility = 0;
        render::Mat4 orient = render::Mat4::identity();
        bool dead = false;
        bool hold = false;
    };
    const std::vector<Corpse>& corpses() const { return corpses_; }
    static bool advance_corpse(Corpse& c, float dt_ms, bool dead_phase);
    struct WarpGhost {
        bool active = false;
        std::string unit_id;
        float x = 0.0f, y = 0.0f;
        bool ok = false;
    };
    const WarpGhost& warp_ghost() const { return warp_ghost_; }
    const formats::Model* warp_ghost_model() const;
    std::string power_visual_actor(const std::string& unit_id) const;
    void arm_warp_placement(std::uint32_t gate, const std::string& unit_id) {
        pending_warp_ = true;
        pending_warp_gate_ = gate;
        pending_warp_unit_ = unit_id;
    }
    void set_models(MeleeModels* models) { models_ = models; }
    const std::map<std::uint32_t, formats::Model>& splat_quads() const { return splat_quads_; }
    struct HudGroupButton {
        int slot = -1;
        int count = 0;
        struct Rect {
            int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
            int w() const { return x1 - x0; }
            int h() const { return y1 - y0; }
        } rect;
        std::string icon;
        std::string unit;
    };
    const std::vector<HudGroupButton>& hud_group_buttons() const { return hud_group_buttons_; }
    void set_control_group(int slot, const std::vector<std::uint32_t>& ids) { groups_.set(slot, ids); }
    struct HudAlertItem {
        std::string text;
        std::string icon;
        float ttl = 0.0f;
        HudGroupButton::Rect rect;
    };
    const std::vector<HudAlertItem>& hud_alert_items() const { return hud_alert_items_; }
    struct HudUnitPane {
        std::string name;
        std::string info;
        std::string type;
        float life_frac = 0.0f;
        HudGroupButton::Rect pane, life_bar;
    };
    const HudUnitPane& hud_unit_pane() const { return hud_unit_pane_; }
    struct HudPortrait {
        std::string unit;
        std::string path;
        float fov_deg = 0.0f;
        HudGroupButton::Rect rect;
        bool drawn = false;
    };
    const HudPortrait& hud_portrait() const { return hud_portrait_; }
    struct HudPageButton {
        int page = 0;
        HudGroupButton::Rect rect;
    };
    int group_page() const { return group_page_; }
    const std::vector<std::uint32_t>& hud_group_pane_ids() const { return hud_group_pane_ids_; }
    const std::vector<HudPageButton>& hud_page_buttons() const { return hud_page_buttons_; }
    std::string alert_sound_id(AlertKind kind, const std::string& alert_override = std::string()) const;
    struct MusicState {
        std::string soundtrack_id;
        std::string sound_id;
        std::string next;
        float delay_min = 0.0f, delay_max = 0.0f;
        float wait_s = 0.0f;
        int voice = -1;
        bool playing = false;
        int requests = 0;
        std::string last_request;
        bool continuous = true;
    };
    void start_music(const std::string& soundtrack_id);
    const MusicState& music() const { return music_; }
    std::string race_music_id(int race_index) const;
    const std::string& last_alert_sound() const { return last_alert_sound_; }
    const std::string& last_effect_sound() const { return last_effect_sound_; }
    const std::string& last_error_sound() const { return last_error_sound_; }
    bool jump_to_last_alert();
    std::vector<EquipmentEntry> equipment_for(int player, const sim::Unit* body = nullptr) const;
    enum class CursorState {
        Normal,
        TargetNormal, TargetAllied, TargetEnemy, TargetInvalid,
        SelectNormal, SelectAllied, SelectEnemy,
        EdgeScrollTop, EdgeScrollBottom, EdgeScrollLeft, EdgeScrollRight,
        EdgeScrollTopLeft, EdgeScrollTopRight, EdgeScrollBottomLeft, EdgeScrollBottomRight,
    };
    CursorState cursor_state() const { return cursor_state_; }
    std::string cursor_id(CursorState s) const;
    int idle_worker_count() const;
    std::uint32_t select_next_idle_worker();
    std::vector<std::uint32_t> army_units() const;
    int army_unit_count() const { return int(army_units().size()); }
    std::vector<std::uint32_t> ready_warp_gates() const;
    int ready_warp_gate_count() const { return int(ready_warp_gates().size()); }

    bool audio_event_audible(const sim::Unit& u) const {
        if (cfg_.human_player < 0) return true;
        if (u.player == cfg_.human_player) return true;
        return world_.visible_to(cfg_.human_player, u);
    }
    const sim::ScanSweep& scan_sweep() const { return world_.scan_sweep(); }

    void set_speed(GameClock::Speed s) { clock_.set_speed(s); }
    GameClock::Speed speed() const { return clock_.speed(); }
    void set_paused(bool p) { clock_.set_paused(p); }
    void toggle_pause() { clock_.toggle_pause(); }
    bool paused() const { return clock_.paused(); }
    struct PauseRect { int x0, y0, x1, y1; };
    bool pause_menu_open() const { return pause_menu_open_; }
    const HudButtons& hud_buttons() const { return hud_buttons_; }
    const HudUiState& hud_ui() const { return hud_ui_; }
    void refresh_hud_buttons(std::uint32_t view_w, std::uint32_t view_h);
    bool on_hud_button(const HudPress& press);
    void hud_dispatch_hotkeys(const render::InputState& in);
    void open_pause_menu();
    bool hud_menu_on_button(const HudPress& press);
    bool hud_minimap_on_button(const HudPress& press);
    bool hud_selection_on_button(const HudPress& press);
    bool hud_topbar_on_button(const HudPress& press);
    bool hud_chat_on_button(const HudPress& press);
    void hud_ui_tick(float dt_s);
    void hud_menu_hotkeys(const render::InputState& in);
    void hud_menu_draw(std::uint32_t view_w, std::uint32_t view_h, std::vector<std::uint8_t>& out);
    bool hud_minimap_take_click(const render::InputState& in, bool have_point, float wx, float wy);
    void hud_minimap_apply(HudContent& content, MinimapBackdrop& backdrop);
    void steal_control_groups(int keep);
    std::vector<std::uint32_t> idle_workers() const;
    bool alert_recall();
    void pause_rects(std::uint32_t view_w, std::uint32_t view_h, PauseRect& resume, PauseRect& quit) const { pause_menu_rects(view_w, view_h, resume, quit); }
    struct PauseRects { PauseRect resume{}, quit{}, save{}, load{}, settings{}; bool has_save = false, has_load = false, has_settings = false; };
    PauseRects pause_rects_all(std::uint32_t view_w, std::uint32_t view_h) const;
    void set_pause_actions(std::function<void()> on_save, std::function<void()> on_load) { on_save_ = std::move(on_save); on_load_ = std::move(on_load); }
    void set_pause_settings_action(std::function<void()> on_settings) { on_settings_ = std::move(on_settings); }
    const MeleeLayout& layout() const { return layout_; }
    const render::ActorDriver& actors() const { return driver_; }
    const std::vector<uint32_t>& selection() const { return selection_; }
    bool geyser_covered(const sim::Unit& u) const;
    void set_selection(std::vector<uint32_t> sel) {
        selection_ = std::move(sel);
        card_page_ = 0;
    }
    const MessageLog& messages() const { return messages_; }
    const sim::ScoreKeeper& score() const { return score_; }
    const SelectionGroups& subgroups() const { return sel_groups_; }
    std::uint32_t cast_lead() const { return human_lead_caster(); }
    render::CameraControl& camera_control() { return camctl_; }
    bool follow_active() const { return follow_.active; }
    void set_ground_fog(bool on) { ground_fog_ = on; }
    void set_reveal_map(bool on) { reveal_map_ = on; }
    void set_fog_type(const std::string& id) { fog_type_ = id; fog_blend_speed_ = -1.0f; }
    bool reveal_map() const { return reveal_map_; }
    MeleeAudio* audio() { return audio_.get(); }
    void set_status_bar_mode(render::UnitOverlay::StatusBarOptions::Mode m) { status_bar_mode_ = m; }
    render::UnitOverlay::StatusBarOptions::Mode status_bar_mode() const { return status_bar_mode_; }
    void set_keep_status_layer(bool keep) { keep_status_layer_ = keep; }
    const std::vector<std::uint8_t>& status_layer() const { return status_layer_; }
    const render::FogField* ground_fog() const {
        const bool bounds_veil = reveal_map_ && map_w_ > 0.0f && map_h_ > 0.0f;
        return (ground_fog_ || bounds_veil) && ground_fog_field_.valid() ? &ground_fog_field_ : nullptr;
    }
    bool selection_centre(float& cx, float& cy) const {
        double sx = 0.0, sy = 0.0;
        int n = 0;
        for (std::uint32_t id : selection_) {
            if (id >= world_.units().size() || !world_.units()[id].alive) continue;
            sx += world_.units()[id].x.to_double();
            sy += world_.units()[id].y.to_double();
            ++n;
        }
        if (!n) return false;
        cx = float(sx / n);
        cy = float(sy / n);
        return true;
    }
    void recall_double_tap(int slot) {
        if (last_recall_slot_ == slot && clock_ms_ - last_recall_ms_ <= kDoubleClickMs) {
            float cx = 0.0f, cy = 0.0f;
            if (control_group_center(groups_, slot, world_, cx, cy)) camctl_.focus = {cx, cy, ground_z_at(cx, cy)};
            last_recall_slot_ = -1;
            return;
        }
        last_recall_slot_ = slot;
        last_recall_ms_ = clock_ms_;
    }
    const render::ActorDriver& driver() const { return driver_; }
    float map_w() const { return map_w_; }
    float map_h() const { return map_h_; }
    const std::vector<std::uint8_t>& minimap_fog() const { return minimap_fog_; }
    const sim::VisionGrid& vision() const { return vision_; }
    int minimap_fog_w() const { return minimap_fw_; }
    int minimap_fog_h() const { return minimap_fh_; }

    std::uint32_t spawn(sim::Unit proto, const std::string& token,
                        const formats::Model* model, sim::Fixed x, sim::Fixed y);
    bool debug_create(const std::string& unit_id, float x, float y, float facing = -1000.0f);
    bool debug_morph(const std::string& from_unit, const std::string& morph_abil);

    MeleeCommandContext command_context();

    void emit_scene(std::vector<render::SceneInstance>& scene);

    const OrderCueState* order_cues() const { return cues_.get(); }
    const formats::Model* unit_ghost_model(const std::string& unit_id) const;

    void set_creep_diffuse(const std::string& rel_path) { creep_diffuse_ = rel_path; }
    const formats::Model& creep_model() const { return creep_model_; }
    const render::FogField& creep_coverage() const { return creep_coverage_; }

    struct FogSnapshot {
        render::SceneInstance inst;
        std::string catalog_id;
        int player = -1;
        float x = 0.0f, y = 0.0f;
        float minimap_radius = 0.0f;
        bool constructing = false;
        float build_progress = 0.0f;
        render::SceneInstance splat;
        bool has_splat = false;
    };
    const std::unordered_map<std::uint32_t, FogSnapshot>& fog_snapshots() const { return fog_snaps_; }
    const sim::CreepGrid& creep_seen() const { return creep_seen_; }
    bool creep_known_fogged() const { return creep_fogged_; }
    void minimap_fog_content(HudContent& content);
    bool cloak_enemy_drawn(const sim::Unit& u) const;
    const render::CloakTransitions& cloak_transitions() const { return cloak_tx_; }
    const render::CloakModelLook& cloak_model_look() const { return cloak_look_; }
    double cloak_clock_ms() const { return cloak_clock_ms_; }

   private:
    void bind_cloak_look();
    render::CloakModelLook cloak_look_;
    render::CloakTransitions cloak_tx_;
    int cloak_look_bound_ = 0;
    double cloak_clock_ms_ = 0.0;
    std::unordered_map<std::uint32_t, FogSnapshot> fog_snaps_;
    std::vector<std::uint8_t> fog_seen_;
    std::unordered_map<std::uint32_t, render::SceneInstance> fog_splat_frame_;
    sim::CreepGrid creep_seen_;
    std::vector<std::int32_t> creep_vis_index_;
    std::int64_t creep_seen_tick_ = -1;
    std::uint64_t creep_seen_dims_ = 0;
    std::uint64_t creep_seen_gen_ = 0;
    bool creep_seen_ready_ = false;
    bool creep_seen_dirty_ = true;
    bool creep_fogged_ = false;
    bool creep_minimap_rgb_set_ = false;
    std::uint8_t creep_minimap_rgb_[3] = {0, 0, 0};
    mutable int cloak_enemy_look_ = -1;
    void refresh_creep_seen();
    std::string creep_diffuse_;
    render::FogField creep_coverage_;
    formats::Model creep_model_;
    std::uint64_t creep_drawn_checksum_ = 0;
    bool creep_model_built_ = false;
    std::uint64_t creep_model_gen_ = 0;

   public:

private:
    std::string human_gas_structure() const;
    std::string human_town_hall() const;
    bool human_is_protoss() const;
    int human_race() const;
    int hall_race() const;
    void ensure_minimap_terrain();
    void update_minimap_fog(int viewed);
    void update_camera(const render::InputState& in, float dt_s);
    void handle_commands(const render::InputState& in, float aspect);
    bool cancel_pending_modal();
    void pause_menu_rects(std::uint32_t view_w, std::uint32_t view_h, PauseRect& resume,
                          PauseRect& quit) const;
    void snapshot_positions(std::vector<render::Vec3>& out) const;
    void rebuild_subgroups();
    void warm_selection_assets(std::uint32_t view_w, std::uint32_t view_h);
    std::vector<std::uint32_t> subgroup_ordered_selection() const;
    std::uint32_t human_lead_caster() const;
    data::TechState human_tech_state(std::uint32_t producer = sim::kNoTarget) const {
        return tech_state_of(cfg_.human_player, producer);
    }
    data::TechState tech_state_of(int player, std::uint32_t producer = sim::kNoTarget) const;

    const std::string& behavior_id_of_tag(std::uint32_t tag) const;
    mutable std::unordered_map<std::uint32_t, std::string> behavior_tags_;
    std::vector<SubmenuCardEntry> merge_selection_submenus(CommandCard& card);
    CommandCard compose_command_card(std::vector<BuildCardEntry>& builds,
                                     std::vector<AbilityCardEntry>& abils,
                                     std::vector<TrainCardEntry>& trains,
                                     std::vector<SubmenuCardEntry>& submenus);
    std::vector<AbilityCardEntry> merge_selection_abilities(CommandCard& card) const;
    bool unpowered_offer_blocked(std::uint32_t id, const std::string& ability) const;
    double folded_train_time(const std::string& ability, const std::string& command,
                             double base) const;

    std::vector<TrainCardEntry> selection_train_entries() const;
    std::vector<TrainCardEntry> merge_selection_train(CommandCard& card) const;
    std::vector<BuildCardEntry> selection_build_entries() const;
    std::vector<BuildCardEntry> merge_selection_build(CommandCard& card) const;
    std::uint32_t selected_training_producer() const;
    std::uint32_t selected_constructing_building() const;
    std::uint32_t selected_liberator() const;
    std::uint32_t selected_liberator_ag() const;
    std::vector<std::uint32_t> selected_merge_templar() const;
    std::vector<std::uint32_t> selected_burrowers(bool burrowed) const;
    bool burrow_command_allowed(std::uint32_t id) const;
    bool liftoff_busy(std::uint32_t id) const;
    std::uint32_t selected_liftoff_structure(bool flying) const;
    std::uint32_t selected_depot(bool lowered) const;
    std::uint32_t selected_loaded_transport() const;

    void maybe_ai_research(int player);
    void run_ai_inject(int player);

    MeleeGameConfig cfg_;
    sim::World world_;
    OrderLog replay_log_;
    std::unique_ptr<LogRecorder> replay_recorder_;
    bool start_grants_done_ = false;
    void apply_start_grants();
    bool start_rally_done_ = false;
    void apply_start_rally();
    bool replay_drive_ = false;
    OrderLog replay_source_;
    std::size_t replay_cursor_ = 0;
    int replay_total_ = 0;
    void inject_replay_orders(int tick);
    void snap_build_site(sim::Fixed& wx, sim::Fixed& wy) const;
    bool build_site_on_grid(sim::Fixed wx, sim::Fixed wy) const;
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
    std::unique_ptr<data::ActorCatalog> actor_cat_;
    mutable std::map<std::string, std::vector<std::pair<std::string, std::string>>> upgrade_model_swaps_;
    const std::vector<std::pair<std::string, std::string>>& upgrade_model_swaps(const std::string& unit_id) const;
    std::map<std::string, render::ActorRules> actor_rules_;
    void build_actor_catalog(const data::Catalog& cat);
    void spawn_corpse(const sim::Unit& u);
    std::string last_alert_sound_;
    std::string last_effect_sound_;
    std::string last_error_sound_;
    CursorState cursor_state_ = CursorState::Normal;
    void update_cursor(const render::InputState& in, Control& ctrl);
    std::string error_sound_id(sim::CmdError e) const;
    std::vector<HudGroupButton> hud_group_buttons_;
    HudButtons hud_buttons_;
    HudRect hud_empty_group_tab_{0, 0, 0, 0};
    HudUiState hud_ui_;
    std::vector<HudAlertItem> hud_alert_items_;
    HudUnitPane hud_unit_pane_;
    std::vector<EquipmentEntry> hud_equipment_;
    HudPortrait hud_portrait_;
    int group_page_ = 0;
    std::vector<std::uint32_t> hud_group_pane_ids_;
    std::vector<HudPageButton> hud_page_buttons_;
    MusicState music_;
    void music_tick(float dt_s);
    float listener_distance_to(const sim::Unit& u) const {
        const float dx = float(u.x.to_double()) - camctl_.focus.x, dy = float(u.y.to_double()) - camctl_.focus.y;
        return std::sqrt(dx * dx + dy * dy);
    }
    static constexpr float kPanHalfWidth = 12.0f;
    float listener_pan_to(const sim::Unit& u) const {
        render::Vec3 r = camctl_.ground_right();
        const float sx = camctl_.mirror_x ? -1.0f : 1.0f;
        const float dx = float(u.x.to_double()) - camctl_.focus.x, dy = float(u.y.to_double()) - camctl_.focus.y;
        const float p = sx * (dx * r.x + dy * r.y) / kPanHalfWidth;
        return p < -1.0f ? -1.0f : (p > 1.0f ? 1.0f : p);
    }
    const render::ActorRules* rules_for(const std::string& catalog_id);

    int card_page_ = 0;
    data::Production production_;
    struct TrainEv {
        bool finish = false;
        std::uint32_t producer = 0;
        std::uint32_t egg = 0;
        std::string produced;
    };
    struct TrainTap : sim::TrainSink {
        std::vector<TrainEv>* out = nullptr;
        void on_train_finish(int, std::uint32_t p, std::uint32_t e, const std::string& u) override {
            if (out) out->push_back({true, p, e, u});
        }
        void on_train_cancel(int, std::uint32_t p, std::uint32_t e, const std::string& u) override {
            if (out) out->push_back({false, p, e, u});
        }
    };
    TrainTap train_tap_;
    std::vector<TrainEv> train_events_;
    void drain_train_events();
    std::string pending_cast_abil_, pending_cast_cmd_;
    AbilTargetMode pending_cast_mode_ = AbilTargetMode::Unit;
    std::unordered_map<std::string, std::string> nova_ability_by_unit_;
    MeleeLayout layout_;
    const formats::Model* ground_ = nullptr;
    const formats::Model* water_ = nullptr;
    const formats::Model* roads_ = nullptr;
    std::array<float, 4> water_rgba_ = {1.0f, 1.0f, 1.0f, 1.0f};
    struct WaterLook {
        bool resolved = false, has = false, receive_shadows = false;
        float color_falloff = 0, specularity = 0, specular_scaler = 0, reflectivity_power = 0, min_reflection = 0;
        float reflection_distortion = 0, refraction_distortion = 0, uv_rotate = 0;
        float tiling[2] = {0.125f, 0.125f};
        float scroll[4] = {0.03f, 0.03f, -0.03f, -0.03f};
        float caustics_falloff = 0, caustics_tiling = 0, caustics_fps = 0;
    } water_look_;
    std::vector<formats::Model> water_caustics_models_;
    std::vector<std::uint8_t> map_water_xml_;
    formats::Model water_fx_model_;
    const formats::Model* water_fx_source_ = nullptr;

    std::vector<uint32_t> selection_;
    bool dragging_ = false;
    bool minimap_drag_ = false;
    MinimapExtent minimap_extent() const;
    float drag_x0_ = 0, drag_y0_ = 0;
    float mouse_x_ = 0, mouse_y_ = 0;
    bool alt_held_ = false;
    render::UnitOverlay::StatusBarOptions::Mode status_bar_mode_ = render::UnitOverlay::StatusBarOptions::Mode::Normal;
    bool keep_status_layer_ = false;
    std::vector<std::uint8_t> status_layer_;
    std::vector<float> status_trail_life_, status_trail_shield_, status_rate_life_, status_rate_shield_;
    int status_trail_tick_ = -1;
    bool framed_ = false;
    float accum_ms_ = 0;
    GameClock clock_;
    int tick_counter_ = 0;
    std::vector<int> ai_own_period_;
    std::vector<int> ai_own_counter_;

    std::vector<render::Vec3> prev_pos_;
    std::vector<render::Vec3> curr_pos_;

    const formats::Model* producer_model_ = nullptr;
    MeleeModels* models_ = nullptr;
    std::vector<const formats::Model*> spawn_worker_model_;
    std::vector<const formats::Model*> spawn_army_model_;
    std::vector<const formats::Model*> spawn_producer_model_;
    casc::Storage* ui_store_ = nullptr;
    const std::vector<formats::PlacedDoodad>* doodads_ = nullptr;
    const std::vector<formats::PlacedDoodad>* fluff_ = nullptr;
    const std::vector<render::SceneInstance>* cliffs_ = nullptr;
    std::vector<float> tier_heights_;
    std::vector<float> ground_heights_;
    std::vector<float> vertex_heights_;
    std::uint32_t vertex_w_ = 0, vertex_h_ = 0;
    std::uint32_t ground_w_ = 0, ground_h_ = 0;
    GroundZ ground_height_{this};
    PickVolumesOf pick_volumes_{this};
    bool select_rules_672_ = true;
    std::vector<PickPose> pick_pose_;
    std::uint64_t pick_frame_ = 0;
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
    int audio_sel_repeat_ = 0;
    bool select_click_ = false;
    IconAtlas icons_;
    IconResampleCache icon_resample_;
    std::set<std::string> warmed_types_;
    bool warming_ = false;
    bool selection_warm_ = true;
    float portrait_hz_ = 0.0f;
    struct PortraitCache {
        std::string path;
        std::uint32_t w = 0, h = 0;
        double at_ms = -1.0;
        std::vector<std::uint8_t> img;
    };
    PortraitCache portrait_cache_;
    bool portrait_async_ = false;
    std::uint32_t prefetch_tick_ = 0;
    bool mem_marked_ = false;
    bool mem_step_marked_ = false;
    std::set<std::string> prefetched_types_;
    bool warm_foreign_ = false;
    bool warm_started_ = false;
    std::vector<std::uint8_t> warm_seen_;
    struct PendingWarm {
        std::uint32_t id = 0;
        std::string type;
    };
    std::vector<PendingWarm> warm_pending_;
    std::uint32_t last_portrait_w_ = 0, last_portrait_h_ = 0;
    std::vector<std::uint8_t> portrait_call(const std::string& pid, int mode);
    bool card_cache_on_ = false;
    struct CardCache {
        bool valid = false;
        std::uint64_t hash = 0;
        std::vector<std::uint32_t> selection;
        int page = 0;
        std::uint32_t producer = 0;
        std::size_t units = 0;
        std::size_t active = 0, groups = 0;
        CommandCard card;
        std::vector<BuildCardEntry> builds;
        std::vector<AbilityCardEntry> abils;
        std::vector<TrainCardEntry> trains;
        std::vector<SubmenuCardEntry> submenus;
    };
    CardCache card_cache_;
    std::size_t card_cache_hits_ = 0;
    CommandCard compose_command_card_cached(std::vector<BuildCardEntry>& builds, std::vector<AbilityCardEntry>& abils,
                                            std::vector<TrainCardEntry>& trains, std::vector<SubmenuCardEntry>& submenus);
    std::unordered_map<std::string, float> display_h_cache_;
    std::unordered_map<const formats::Model*, WireMask> wire_mask_cache_;
    std::unordered_map<const formats::Model*, float> model_diam_cache_;
    std::unordered_map<const formats::Model*, bool> self_lit_cache_;
    std::unordered_map<const formats::Model*, float> ref_z_cache_;
    void clear_model_caches() {
        wire_mask_cache_.clear();
        model_diam_cache_.clear();
        self_lit_cache_.clear();
        ref_z_cache_.clear();
    }
    render::OverlayFont overlay_font_;
    bool overlay_font_tried_ = false;
    std::map<std::string, render::OverlayFont> style_faces_;
    const render::OverlayFont* style_face(const std::string& font_path);
    void ensure_overlay_font();
    ConsoleTexture console_tex_;
    ConsoleRenderer console_renderer_;
    PortraitRenderer portrait_renderer_;
    const HudAuthoredRects* hud_authored_ = nullptr;
    bool console_tried_ = false;
    std::uint32_t console_view_w_ = 0, console_view_h_ = 0;
    void ensure_console_texture(std::uint32_t view_w = 0, std::uint32_t view_h = 0);
    static constexpr const char* kHudReadoutStyle = "ResourcePanel";
    const ui::StyleSet* hud_style_set();
    ui::StyleSet hud_style_;
    bool hud_style_tried_ = false;

    ConsolePanel console_view() const;
    void refresh_hud_layout(std::uint32_t view_w, std::uint32_t view_h);
    std::size_t bound_count_ = 0;
    std::vector<std::string> bound_cat_;
    std::vector<std::uint8_t> bound_building_;

    float map_w_ = 0, map_h_ = 0;
    float playable_x0_ = -1.0f, playable_y0_ = -1.0f, playable_x1_ = -1.0f, playable_y1_ = -1.0f;
    float cam_left_ = 0.0f, cam_right_ = 0.0f;
    float fps_smooth_ = 0.0f;
    sim::VisionGrid vision_;
    render::FogOverlay fog_;
    std::vector<std::uint8_t> minimap_terrain_;
    int minimap_tw_ = 0, minimap_th_ = 0;
    bool minimap_terrain_baked_ = false;
    bool minimap_terrain_authored_ = false;
    bool minimap_terrain_cropped_ = false;
    std::vector<std::uint8_t> minimap_fog_;
    int minimap_fw_ = 0, minimap_fh_ = 0;
    render::FogField ground_fog_field_;
    std::vector<float> fog_blend_;
    float fog_dt_s_ = 0.0f;
    float fog_blend_speed_ = -1.0f;
    bool ground_fog_ = false;
    bool reveal_map_ = false;
    std::string fog_type_ = "Dark";
    bool fog_hidden_black_ = false;
    std::uint8_t fog_veil_alpha_ = 165;
    render::UnitOverlay unit_overlay_;
    ControlGroups groups_;
    MessageLog messages_;
    AlertWatcher alerts_;
    sim::ScoreKeeper score_;
    std::vector<std::uint8_t> death_seen_;
    std::map<std::uint32_t, formats::Model> splat_quads_;
    std::set<std::uint32_t> splat_none_;
    const formats::Model* splat_quad_for(const sim::Unit& u);
    std::map<std::uint32_t, formats::Model> power_quads_;
    std::set<std::uint32_t> power_quad_none_;
    std::map<std::uint32_t, std::array<float, 2>> power_quad_at_;
    mutable std::map<std::string, std::string> power_visual_cache_;
    std::vector<std::uint8_t> constructing_seen_;
    WarpGhost warp_ghost_;
    std::map<std::string, const formats::Model*> warp_build_model_;
    bool draws_authored_warp_in(const std::string& catalog_id, const formats::Model* drawn);
    const formats::Model* power_quad_for(const sim::Unit& u);
    std::unordered_map<std::string, std::optional<std::array<float, 3>>> tint_cache_;
    std::unordered_map<std::string, float> flying_height_cache_;
    float display_height(const std::string& id, bool flying_variant);
    struct DoodadDraw {
        bool ready = false;
        formats::PlacedDoodad src;
        const formats::Model* model = nullptr;
        render::Mat4 world;
        std::uint64_t seed = 0;
        int seq = -1;
        const render::VariationTakes* takes = nullptr;
        render::VariationWalk walk;
        std::uint8_t thin_at = 0;
    };
    std::vector<DoodadDraw> doodad_draw_;
    std::vector<DoodadDraw> fluff_draw_;
    std::unordered_map<const formats::Model*, render::VariationTakes> stand_takes_;
    std::optional<data::Catalog::HallucinationLook> halluc_look_;
    bool halluc_look_resolved_ = false;
    std::uint32_t halluc_tag_ = 0;
    std::vector<Corpse> corpses_;
    struct ImpactFx {
        const formats::Model* model = nullptr;
        int seq = -1;
        float time_ms = 0.0f, length_ms = 0.0f;
        float x = 0.0f, y = 0.0f, z = 0.0f;
        float yaw = 0.0f;
        float scale = 1.0f;
        int player = -1;
    };
    std::vector<ImpactFx> impact_fx_;
    struct EffectDoodad {
        const formats::Model* model = nullptr;
        std::string model_id;
        int seq[3] = {-1, -1, -1};
        int phase = 0;
        float time_ms = 0.0f;
        float x = 0.0f, y = 0.0f, z = 0.0f, yaw = 0.0f;
        int player = -1;
    };
    std::vector<EffectDoodad> effect_doodads_;
    void spawn_effect_doodads(const std::string& abil_id, sim::Fixed px, sim::Fixed py, int player);
    EffectActorHost fx_host_;
    std::unordered_map<std::string, std::string> addon_ids_;
    std::vector<float> addon_total_ms_;
    struct BeamFx {
        const formats::Model* model = nullptr;
        std::uint32_t attacker = 0xFFFFFFFFu, target = 0xFFFFFFFFu;
        std::uint8_t slot = 0;
        bool continuous = false, heal = false;
        float scale = 1.0f;
        int seq[3] = {-1, -1, -1};
        int phase = 0;
        float time_ms = 0.0f;
        float idle_ms = 0.0f;
        float keep_ms = 1000.0f;
        int beam_bone = -1;
        int player = -1;
        float to[3] = {0, 0, 0};
    };
    std::vector<BeamFx> beams_;
    std::unordered_map<std::string, std::string> recharge_beam_;
    std::unordered_map<std::uint32_t, std::string> effect_tag_names_;
    struct BurnFx {
        int state = -1;
        std::vector<int> points;
        double start_ms = 0.0;
    };
    std::unordered_map<std::uint32_t, BurnFx> burning_;
    void beam_refresh(std::uint32_t attacker, std::uint32_t target, std::uint8_t slot, const std::string& key,
                      bool heal, float keep_ms);
    bool unit_attach_world(const sim::Unit& u, const char* first, const char* second, float out[3]);
    std::uint32_t hover_unit_ = 0xFFFFFFFFu;
    std::vector<double> flash_hp_;
    std::vector<double> flash_ms_;
    std::vector<std::size_t> upg_seen_;
    std::vector<float> upg_flash_s_;
    SelectionGroups sel_groups_;
    std::vector<std::uint32_t> sel_groups_members_;
    CameraBookmarks bookmarks_;
    render::CameraFollow follow_;
    bool user_scrolled_ = false;
    double kDoubleClickMs = 500.0;

public:
    void set_double_click_ms(double ms) { if (ms > 0.0) kDoubleClickMs = ms; }

private:
    double clock_ms_ = 0.0;
    struct SeenShot { render::SceneInstance inst; double last_ms = 0.0; float trail_s = 0.0f; };
    std::map<std::string, SeenShot> shots_seen_;
    double last_click_ms_ = -1e9;
    float last_click_x_ = -1e9f, last_click_y_ = -1e9f;
    int last_recall_slot_ = -1;
    double last_recall_ms_ = -1e9;
    std::size_t town_camera_next_ = 0;
    MatchResult::Outcome outcome_ = MatchResult::Outcome::Ongoing;
    int surrendered_player_ = -1;
    int stalemate_ticks_ = 0;
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
        int producer_gas = 0;
        int army_cost = 50;
        sim::Fixed worker_build_time = sim::Fixed::from_int(12);
        sim::Fixed supply_build_time = sim::Fixed::from_int(21);
        sim::Fixed producer_build_time = sim::Fixed::from_int(46);
    };
    HumanBuildConfig human_build_;
    bool pending_attack_ = false;
    bool pending_patrol_ = false;
    bool pending_move_ = false;
    bool consume_modal_target(sim::Fixed wx, sim::Fixed wy, bool queued = false);
    bool modal_shift_kept_ = false;
    bool card_smart_click(const std::string& tag, const Control& ctrl, const ScreenPoint& p, bool queued);
    bool human_load_all(const std::string& abil_id);
    bool pending_defender_ = false;
    std::uint32_t pending_liberator_ = sim::kNoTarget;
    bool pending_land_ = false;
    std::uint32_t pending_land_unit_ = sim::kNoTarget;
    std::string pending_land_addon_;
    bool pending_unload_ = false;
    std::uint32_t pending_unload_unit_ = sim::kNoTarget;
    bool pending_build_ = false;
    std::uint32_t build_caster_ = sim::kNoTarget;
    bool pending_refinery_ = false;
    bool pending_warp_ = false;
    std::uint32_t pending_warp_gate_ = sim::kNoTarget;
    std::string pending_warp_unit_;
    sim::Unit build_proto_;
    int build_cost_ = 0;
    int build_gas_cost_ = 0;
    sim::Fixed build_time_;
    bool build_valid_ = false;
    sim::Fixed build_px_, build_py_;
    struct PlacementGhost {
        bool active = false;
        std::string unit_id;
        float x = 0.0f, y = 0.0f;
        bool ok = false;
        std::vector<std::pair<int, int>> cells;
        std::vector<char> cell_ok;
    };
    PlacementGhost ghost_;
    std::unordered_map<std::uint32_t, std::string> morph_abil_;
    std::unordered_map<std::uint32_t, std::pair<std::int32_t, std::int32_t>> morph_paid_;
    std::int32_t morph_as_player_ = -1;
    std::unordered_map<std::uint32_t, bool> morph_autocast_was_morphing_;
    std::unordered_map<std::string, bool> morph_autocast_off_;
    struct MorphAutocast {
        std::string abil;
        std::string requirement;
        std::vector<std::uint32_t> not_behaviors;
    };
    std::unordered_map<std::string, std::vector<MorphAutocast>> morph_autocast_cache_;
    struct MorphShow {
        const formats::Model* model = nullptr;
        int seq = -1;
        float len_ms = 0.0f;
        double delay_s = 0.0;
        int stand_seq = -1;
        float stand_ms = 0.0f;
    };
    bool morph_transition_models_ = false;
    bool morph_upgrade_fold_ = false;
    bool morph_lock_order_ = false;
    bool morph_swap_at_data_ = false;
    bool morph_start_delay_ = std::getenv("OMC_MORPH_START_DELAY") != nullptr && std::string(std::getenv("OMC_MORPH_START_DELAY")) == "1";
    bool morph_behaviors_sim_ = std::getenv("OMC_MORPH_BEHAVIOR_SIM_OLD") == nullptr;
    bool upgrade_fold_new_ = false;
    bool timed_burrow_ = false;
    bool ai_paused_ = false;
    bool ai_inject_ = false;
    int ai_injects_ = 0;
    std::map<std::string, std::string> inject_abil_;
    bool timed_depot_ = false;
    bool train_gate_strict_ = false;
    bool hall_needs_builder_ = false;
    bool group_self_cast_ = false;
    bool group_cast_cell_ = false;
    bool best_unit_pick_ = false;
    bool morph_cocoon_models_ = false;
    bool zone_markers_ = false;
    bool actor_game_clock_ = false;
    std::unordered_map<std::uint32_t, std::pair<std::string, int>> zone_marker_;
    std::string zone_marker_of(const std::string& unit_id) const;
    bool construction_child_team_ = true;
    std::unordered_map<std::string, MorphShow> morph_show_;
    const MorphShow* morph_show(const sim::Unit& u);
    struct QueuedBuild {
        std::uint32_t worker;
        sim::Fixed x, y;
        sim::Unit proto;
        int cost;
        int gas;
        sim::Fixed time;
        bool is_producer;
    };
    std::vector<QueuedBuild> build_queue_;
    sim::BuildVerb human_build_verb_ = sim::BuildVerb::Committed;
    std::shared_ptr<OrderCueState> cues_;
    OrderCueState& cues();
    void cue_confirm_point(float x, float y, bool attack);
    void cue_confirm_target(std::uint32_t target);
    void cue_after_order(bool have_point, float x, float y, std::uint32_t target);
    void cue_cursor_point(bool have_point, float x, float y);
    void cue_build_issued(std::uint32_t worker, sim::Fixed x, sim::Fixed y, const std::string& unit_id);
    void emit_order_cues(std::vector<render::SceneInstance>& scene);
    void draw_order_paths(std::uint32_t view_w, std::uint32_t view_h, std::vector<std::uint8_t>& out, const Control& ctrl,
                          const ConsolePanel* clip);
    bool prev_num_[10] = {};

    bool pause_menu_open_ = false;
    std::function<void()> on_save_, on_load_, on_settings_;
    bool prev_esc_ = false;

};

}
