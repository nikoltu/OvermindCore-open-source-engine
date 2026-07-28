#pragma once

#include <string>
#include <vector>

#include "sc2/render/actor.hpp"

namespace sc2::data {
struct CActorUnit;
}

namespace sc2::render {

struct ActorRules {
    std::vector<std::string> by_state[5];
    bool valid = false;

    const std::vector<std::string>& tokens(AnimState s) const { return by_state[int(s)]; }

    static ActorRules from_actor(const data::CActorUnit& actor);
};

}
