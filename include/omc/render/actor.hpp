#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "omc/formats/m3.hpp"
#include "omc/render/anim.hpp"
#include "omc/sim/world.hpp"

namespace omc::render {

struct ActorRules;

struct AudioSink;

enum class AnimState {
    Birth, Stand, Walk, Attack, Death, Gather, Cast,
    FlyStart, FlyStand, FlyWalk, FlyEnd
};
inline constexpr int kAnimStateCount = 11;

struct AnimSlot {
    std::string name;
    std::vector<std::string> props;
    int sequence = -1;
    float time_ms = 0;
    bool looping = false;
    int priority = 0;
    std::string group;
    std::vector<std::string> closing;
    std::vector<std::string> content;
    bool closing_phase = false;
    bool content_once = false;
    bool content_playing = false;
    bool stop_pending = false;
    bool global = false;
};

struct ActorEventRule {
    std::string head, subject, sub;
    std::vector<std::pair<std::string, std::string>> filters;
    std::string verb;
    std::vector<std::string> args;
    std::string target;
};

struct ActorEvent {
    std::string head, subject, sub;
    std::vector<std::pair<std::string, std::string>> ctx;
};

struct ActorTimer {
    std::string name;
    float left_ms = 0;
};

struct ActorInstance;
struct ActorChild {
    std::string name;
    std::vector<ActorEventRule> rules;
    std::shared_ptr<ActorInstance> rig;
    std::vector<std::vector<std::string>> ladder;
    float scale[3] = {1.0f, 1.0f, 1.0f};
    float yaw = 0.0f;
    int construction_stage = -1;
    bool finishing = false;
    float finish_len_ms = 0.0f;
};

struct ActorInstance {
    const formats::Model* model = nullptr;
    const ActorRules* rules = nullptr;
    uint64_t seed = 0;
    float facing = 0.0f;
    bool facing_known = false;
    AnimState baseline = AnimState::Stand;
    AnimState oneshot = AnimState::Birth;
    bool has_oneshot = true;
    bool dead = false;
    float clock = 0;
    static constexpr float kDefaultBlendMs = 200.0f;
    float blend_time_ms = kDefaultBlendMs;
    bool blend_time_set = false;
    int prev_seq = -1;
    float prev_time_ms = 0;
    float blend_left_ms = 0, blend_total_ms = 0;
    int noted_seq = -1;
    bool turret_bound = false;
    bool reach_bound = false;
    int reach_tip = -1, reach_root = -1, reach_weapon = -1;
    bool reach_live = false;
    float reach_last_x = 0.0f, reach_last_y = 0.0f, retract_t0 = -1.0f, retract_from_x = 0.0f, retract_from_y = 0.0f, retract_dist = 0.0f;
    std::uint32_t reach_target_id = 0;
    bool reach_has_target = false;
    float retract_delay = 0.0f, retract_v0 = 0.0f, retract_acc = 0.0f, retract_vmax = 0.0f;
    float ret_lte[2] = {0.0f, 0.0f}, ret_v0[2] = {0.0f, 0.0f}, ret_acc[2] = {0.0f, 0.0f}, ret_vmax[2] = {0.0f, 0.0f};
    int ret_n = 0;
    int turret_bone = -1;
    float turret_yaw = 0.0f;
    float turret_goal = 0.0f;
    bool turret_has_goal = false;
    float turret_rate_rad = 0.0f, turret_arc_rad = 0.0f, turret_start_rad = 0.0f, turret_idle_rate_rad = 0.0f;
    bool turret_spin = false;
    bool turret_hold = false;
    bool turret_had_goal = false;
    float turret_fidget_min_s = 0.0f, turret_fidget_max_s = 0.0f, turret_fidget_turn_rad = 0.0f, turret_fidget_rate_rad = 0.0f;
    float turret_fidget_chance = 0.0f;
    float turret_fidget_wait_s = -1.0f;
    float turret_fidget_offset = 0.0f;
    bool turret_fidget_turning = false;
    std::uint32_t turret_fidget_rng = 0;
    void note_pose(int seq, float time_ms);
    float blend_weight() const {
        return (prev_seq >= 0 && blend_total_ms > 0.0f && blend_left_ms > 0.0f) ? blend_left_ms / blend_total_ms : 0.0f;
    }
    float walk_scale = 1.0f;
    uint32_t swing = 0;
    int cached_seq[kAnimStateCount] = {-2, -2, -2, -2, -2, -2, -2, -2, -2, -2, -2};
    uint32_t idle_loops = 0;
    uint32_t walk_loops = 0;
    uint32_t slot_plays = 0;
    uint32_t oneshot_plays = 0;
    std::vector<std::string> paused_names;
    bool paused_all = false;
    bool anim_paused(const std::string& slot) const;
    std::vector<std::string> anim_groups;
    std::vector<std::string> birth_clips;

    std::vector<formats::M3Event> last_events;

    std::vector<AnimSlot> slots;

    std::vector<ActorEventRule> event_rules;

    int model_variation = -1;

    void bind_event_rules(std::vector<ActorEventRule> rules);
    int apply_birth_groups(const std::string& unit_id);
    std::vector<ActorTimer> timers;
    std::vector<ActorEvent> queued;
    std::size_t bus_overflow = 0;
    std::size_t unhandled_verbs = 0;
    float bus_tint[3] = {1.0f, 1.0f, 1.0f};
    bool bus_tint_set = false;
    struct TintLayer {
        std::string key;
        float rgb[3] = {1.0f, 1.0f, 1.0f};
    };
    std::vector<TintLayer> tint_layers;
    float tint_from[3] = {1.0f, 1.0f, 1.0f};
    float tint_to[3] = {1.0f, 1.0f, 1.0f};
    float tint_blend_ms = 0.0f;
    float tint_left_ms = 0.0f;
    void retarget_tint(float blend_ms);
    void advance_tint(float dt_ms);
    bool host_owned = false;
    bool self_destroyed = false;
    void bracket_closed(const std::string& slot);
    void close_bracket_now(const std::string& slot);
    void settle_brackets();
    float bus_scale = 1.0f;
    bool bus_scale_set = false;
    std::string bus_model_swap;
    std::size_t unanswerable_filters = 0;
    std::map<std::string, int> status;
    void set_status(const std::string& name, int value, bool raise_set);
    std::size_t rules_fired = 0;

    void raise(const std::string& head, const std::string& subject = std::string(),
               const std::string& sub = std::string(),
               std::vector<std::pair<std::string, std::string>> ctx = {});
    void pump(float dt_ms);
    bool timer_armed(const std::string& name) const;

    std::vector<ActorChild> children;
    std::function<std::vector<ActorEventRule>(const std::string&)> child_rules;
    bool has_child(const std::string& name) const;
    std::string harvest_abil;

    int construction_stage = -1;
    AnimState current() const { return has_oneshot ? oneshot : baseline; }
    void anim_play(const std::string& name, const std::vector<std::string>& props,
                   bool looping = true, int priority = 0);
    void anim_clear(const std::string& name);
    bool anim_playing(const std::string& name) const;
    const AnimSlot* top_slot() const;
    AnimSlot* top_slot();
    int current_sequence();
    float current_time() const;
    ClipLayers global_layers() const;
    const std::vector<std::string>& tokens_of(AnimState s) const;
    int sequence_of(AnimState s);
    void trigger(AnimState s);
    void set_air_activity(bool moving);
    void set_activity(bool moving, bool gathering);
    void advance(float dt_ms);
    Mat4 orient() const { return rotation_z(facing + 1.5707963f); }

    std::vector<Mat4> pose_now();
    bool attachment(const std::string& name, Mat4& out);
};

struct ActorDriver {
    std::function<std::vector<ActorEventRule>(const std::string&)> unit_rules;
    std::function<std::vector<ActorEventRule>(const std::string&)> sub_actor_rules;
    std::function<std::string(const std::string&)> harvest_ability;

    struct BurrowPair {
        std::string down_ability;
        std::string up_ability;
        std::string down_key;
        std::string up_key;
        std::string variant;
        double down_ms = 0;
        double up_ms = 0;
    };
    std::function<BurrowPair(const std::string&)> burrow_pair;
    std::function<BurrowPair(const std::string&)> depot_pair;
    std::function<std::string(uint32_t)> morph_target;
    std::function<int(int, const std::string&)> validate_player;
    std::map<int, std::size_t> upgrades_seen;

    struct Shadow {
        bool known = false;
        bool alive = true;
        sim::Fixed x, y, cooldown;
        std::uint32_t cast_count = 0;
        bool flying = false;
        bool gathering = false;
        int res_level = -1;
        std::int32_t res_cap = 0;
        int res_depleted = -1;
        int power_down = -1;
        bool burrowed = false;
        float burrow_finish_ms = -1.0f;
        bool burrow_finish_down = false;
        bool burrow_sim_timed = false;
        bool lowered = false;
        float depot_finish_ms = -1.0f;
        bool depot_finish_down = false;
        bool depot_sim_timed = false;
        std::string catalog_id;
        std::string morph_to;
        bool constructing = false;
        bool moving = false;
        bool created_raised = false;
        bool destroyed_raised = false;
    };
    std::vector<Shadow> shadows;
    std::vector<ActorInstance> actors;
    std::vector<const formats::Model*> models;
    std::vector<const ActorRules*> rules;
    const formats::Model* default_model = nullptr;
    const ActorRules* default_rules = nullptr;
    AudioSink* audio = nullptr;

    void set_model(uint32_t unit_id, const formats::Model* m) {
        if (unit_id >= models.size()) models.resize(unit_id + 1, nullptr);
        models[unit_id] = m;
    }
    void set_rules(uint32_t unit_id, const ActorRules* r) {
        if (unit_id >= rules.size()) rules.resize(unit_id + 1, nullptr);
        rules[unit_id] = r;
    }
    void set_spawn_facing(uint32_t unit_id, float yaw) {
        if (unit_id >= spawn_facing.size()) {
            spawn_facing.resize(unit_id + 1, 0.0f);
            spawn_facing_set.resize(unit_id + 1, 0);
        }
        spawn_facing[unit_id] = yaw;
        spawn_facing_set[unit_id] = 1;
    }
    std::vector<float> spawn_facing;
    std::vector<char> spawn_facing_set;
    void observe(const sim::World& world, float dt_ms, bool sim_ticked = true);
};

}
