#pragma once

#include <string>
#include <vector>

#include "sc2/render/actor.hpp"

namespace sc2::data {
struct CActorUnit;
}

namespace sc2::render {

bool anim_arg_is_flags(const std::string& arg);

struct AnimArgs {
    std::vector<std::string> props;
    bool looping = true;
};
AnimArgs anim_args_of(const std::vector<std::string>& args);

struct ActorRules {
    struct Slot {
        std::string name;
        std::vector<std::string> props;
        bool looping = true;
        std::string open_event;
        std::vector<std::string> close_events;
    };
    std::vector<Slot> slots;

    std::vector<std::string> by_state[kAnimStateCount];
    bool valid = false;

    const std::vector<std::string>& tokens(AnimState s) const { return by_state[int(s)]; }

    static ActorRules from_actor(const data::CActorUnit& actor);
};

}
