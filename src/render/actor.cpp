#include "sc2/render/actor.hpp"

#include "sc2/render/actor_rules.hpp"

namespace sc2::render {

namespace {
const std::vector<std::string>& tokens_for(AnimState s) {
    static const std::vector<std::string> birth{"Birth"}, stand{"Stand"}, walk{"Walk"},
        attack{"Attack"}, death{"Death"};
    switch (s) {
        case AnimState::Birth: return birth;
        case AnimState::Walk: return walk;
        case AnimState::Attack: return attack;
        case AnimState::Death: return death;
        case AnimState::Stand: default: return stand;
    }
}
float clip_len(const formats::Model& m, int seq) {
    if (seq < 0 || seq >= int(m.sequences.size())) return 0;
    float l = float(m.sequences[seq].length_ms());
    return l > 1 ? l : 1000.0f;
}
}

const std::vector<std::string>& ActorInstance::tokens_of(AnimState s) const {
    if (rules && rules->valid && !rules->tokens(s).empty()) return rules->tokens(s);
    return tokens_for(s);
}

int ActorInstance::sequence_of(AnimState s) {
    int i = int(s);
    if (cached_seq[i] == -2) {
        int r = model ? select_sequence(*model, tokens_of(s), seed) : -1;
        if (r < 0 && model)
            r = select_sequence(*model, tokens_of(AnimState::Stand), seed);
        if (r < 0 && model && !model->sequences.empty()) r = 0;
        cached_seq[i] = r;
    }
    return cached_seq[i];
}

void ActorInstance::trigger(AnimState s) {
    if (dead) return;
    if (s == AnimState::Death) dead = true;
    oneshot = s;
    has_oneshot = true;
    clock = 0;
}

void ActorInstance::set_moving(bool moving) {
    if (dead) return;
    baseline = moving ? AnimState::Walk : AnimState::Stand;
}

void ActorInstance::advance(float dt_ms) {
    if (!model) return;
    clock += dt_ms;
    int seq = sequence_of(current());
    float len = clip_len(*model, seq);
    if (has_oneshot) {
        if (current() == AnimState::Death) {
            if (clock > len) clock = len;
        } else if (clock >= len) {
            has_oneshot = false;
            clock = 0;
        }
    } else if (clock >= len) {
        clock -= len * float(int(clock / len));
    }
}

std::vector<Mat4> ActorInstance::pose_now() {
    if (!model) return {};
    return pose(*model, uint32_t(sequence_of(current())), clock);
}

void ActorDriver::observe(const sim::World& world, float dt_ms) {
    const std::vector<sim::Unit>& units = world.units();
    if (shadows.size() < units.size()) shadows.resize(units.size());
    if (actors.size() < units.size()) actors.resize(units.size());

    for (size_t i = 0; i < units.size(); ++i) {
        const sim::Unit& u = units[i];
        Shadow& s = shadows[i];
        ActorInstance& a = actors[i];

        if (!s.known) {
            a.model = (u.id < models.size() && models[u.id]) ? models[u.id] : default_model;
            a.rules = (u.id < rules.size() && rules[u.id]) ? rules[u.id] : default_rules;
            a.seed = u.id + 1;
            a.trigger(AnimState::Birth);
            s.known = true;
            s.alive = u.alive;
            s.x = u.x; s.y = u.y; s.cooldown = u.cooldown;
        }

        if (s.alive && !u.alive) a.trigger(AnimState::Death);

        if (u.cooldown > s.cooldown && s.cooldown == sim::Fixed::from_int(0) && u.alive)
            a.trigger(AnimState::Attack);

        bool moving = (u.x != s.x || u.y != s.y);
        a.set_moving(moving);

        a.advance(dt_ms);

        s.alive = u.alive;
        s.x = u.x; s.y = u.y; s.cooldown = u.cooldown;
    }
}

}
