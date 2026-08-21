#pragma once

#include <cstdint>
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

    AnimState current() const { return has_oneshot ? oneshot : baseline; }
    const std::vector<std::string>& tokens_of(AnimState s) const;
    int sequence_of(AnimState s);
    void trigger(AnimState s);
    void set_moving(bool moving);
    void set_air_activity(bool moving);
    void set_activity(bool moving, bool gathering);
    void advance(float dt_ms);
    Mat4 orient() const { return rotation_z(facing); }

    std::vector<Mat4> pose_now();
    bool attachment(const std::string& name, Mat4& out);
};

struct ActorDriver {
    struct Shadow {
        bool known = false;
        bool alive = true;
        sim::Fixed x, y, cooldown;
        std::uint32_t cast_count = 0;
        bool flying = false;
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
    void observe(const sim::World& world, float dt_ms, bool sim_ticked = true);
};

}
