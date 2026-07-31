#include "sc2/render/actor.hpp"

#include "sc2/render/actor_rules.hpp"
#include "sc2/render/audio_sink.hpp"

namespace sc2::render {

namespace {
const std::vector<std::string>& tokens_for(AnimState s) {
    static const std::vector<std::string> birth{"Birth"}, stand{"Stand"}, walk{"Walk"},
        attack{"Attack"}, death{"Death"},
        gather{"Work", "Stand Gather", "Gather"};
    switch (s) {
        case AnimState::Birth: return birth;
        case AnimState::Walk: return walk;
        case AnimState::Attack: return attack;
        case AnimState::Death: return death;
        case AnimState::Gather: return gather;
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

void ActorInstance::set_activity(bool moving, bool gathering) {
    if (dead) return;
    baseline = moving ? AnimState::Walk
                      : (gathering ? AnimState::Gather : AnimState::Stand);
}

void ActorInstance::advance(float dt_ms) {
    last_events.clear();
    if (!model) return;
    const int seq = sequence_of(current());
    const float len = clip_len(*model, seq);
    const float from = clock;
    const float to = clock + dt_ms;
    if (to <= len) {
        collect_events(*model, uint32_t(seq), from, to, last_events);
    } else {
        collect_events(*model, uint32_t(seq), from, len, last_events);
        if (!has_oneshot) {
            float rem = to - len;
            if (rem > len) rem = len;
            collect_events(*model, uint32_t(seq), 0.0f, rem, last_events);
        }
    }
    clock = to;
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

bool ActorInstance::attachment(const std::string& name, Mat4& out) {
    if (!model) return false;
    return attachment_transform(*model, name, uint32_t(sequence_of(current())), clock, out);
}

void ActorDriver::observe(const sim::World& world, float dt_ms, bool sim_ticked) {
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
            if (a.model && select_sequence(*a.model, a.tokens_of(AnimState::Birth), a.seed) >= 0)
                a.trigger(AnimState::Birth);
            else
                a.has_oneshot = false;
            s.known = true;
            s.alive = u.alive;
            s.x = u.x; s.y = u.y; s.cooldown = u.cooldown;
        }

        if (sim_ticked) {
            if (s.alive && !u.alive) a.trigger(AnimState::Death);

            if (u.cooldown > s.cooldown && s.cooldown == sim::Fixed::from_int(0) && u.alive)
                a.trigger(AnimState::Attack);

            bool moving = (u.x != s.x || u.y != s.y);
            a.set_activity(moving, u.mine_cd.raw() > 0);

            s.alive = u.alive;
            s.x = u.x; s.y = u.y; s.cooldown = u.cooldown;
        }

        a.advance(dt_ms);
        if (audio && !a.last_events.empty())
            emit_event_cues(*audio, a.last_events, u.id);
    }
}

}
