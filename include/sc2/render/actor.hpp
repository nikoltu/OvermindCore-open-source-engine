#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "sc2/formats/m3.hpp"
#include "sc2/render/anim.hpp"
#include "sc2/sim/world.hpp"

namespace sc2::render {

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
};

struct ActorEventRule {
    std::string head, subject, sub;
    std::vector<std::pair<std::string, std::string>> filters;
    std::string verb;
    std::vector<std::string> args;
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
    float walk_scale = 1.0f;
    uint32_t swing = 0;
    int cached_seq[kAnimStateCount] = {-2, -2, -2, -2, -2, -2, -2, -2, -2, -2, -2};

    std::vector<formats::M3Event> last_events;

    std::vector<AnimSlot> slots;

    std::vector<ActorEventRule> event_rules;
    std::vector<ActorTimer> timers;
    std::vector<ActorEvent> queued;
    std::size_t bus_overflow = 0;
    std::size_t unhandled_verbs = 0;
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

    struct Shadow {
        bool known = false;
        bool alive = true;
        sim::Fixed x, y, cooldown;
        std::uint32_t cast_count = 0;
        bool flying = false;
        bool gathering = false;
        bool burrowed = false;
        float burrow_finish_ms = -1.0f;
        bool burrow_finish_down = false;
        bool lowered = false;
        float depot_finish_ms = -1.0f;
        bool depot_finish_down = false;
        std::string catalog_id;
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
