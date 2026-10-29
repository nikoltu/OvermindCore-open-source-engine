#pragma once

#include <cstdint>

namespace omc::render {

enum class AnimMsg {
    Spawn,
    Death,
    MoveStart,
    MoveStop,
    GatherStart,
    GatherStop,
    AttackStart,
    AttackStop,
};

struct AnimMessage {
    AnimMsg kind;
    uint32_t unit = 0;
};

}
