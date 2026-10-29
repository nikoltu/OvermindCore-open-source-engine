#pragma once

#include <string>
#include <vector>

#include "omc/render/anim_layers.hpp"
#include "omc/render/anim_msg.hpp"

namespace omc::render {

enum class AnimAction {
    SetBaseline,
    PlayOneShot,
    BracketStart,
    BracketStop,
};

struct AnimRule {
    AnimMsg on;
    AnimAction action;
    std::vector<std::string> tokens;
    bool latch = false;
    bool loops = false;
};

struct AnimEventTable {
    std::vector<AnimRule> rules;

    void apply(AnimMsg msg, AnimLayerStack& stack) const {
        if (stack.latched) return;
        for (const AnimRule& r : rules) {
            if (r.on != msg) continue;
            switch (r.action) {
                case AnimAction::SetBaseline:
                    stack.baseline = r.tokens;
                    break;
                case AnimAction::PlayOneShot:
                    stack.oneshot = r.tokens;
                    stack.oneshot_active = true;
                    stack.oneshot_loops = r.loops;
                    if (r.latch) stack.latched = true;
                    break;
                case AnimAction::BracketStart:
                    stack.bracket_tokens.content = r.tokens;
                    stack.bracket.begin();
                    break;
                case AnimAction::BracketStop:
                    stack.bracket.release();
                    break;
            }
        }
    }
};

inline AnimEventTable default_event_table() {
    AnimEventTable t;
    t.rules = {
        {AnimMsg::Spawn, AnimAction::PlayOneShot, {"Birth"}, false, false},
        {AnimMsg::Death, AnimAction::PlayOneShot, {"Death"}, true, true},
        {AnimMsg::MoveStart, AnimAction::SetBaseline, {"Walk"}, false, false},
        {AnimMsg::MoveStop, AnimAction::SetBaseline, {"Stand"}, false, false},
        {AnimMsg::GatherStart, AnimAction::SetBaseline, {"Work", "Stand Gather", "Gather"}, false, false},
        {AnimMsg::GatherStop, AnimAction::SetBaseline, {"Stand"}, false, false},
        {AnimMsg::AttackStart, AnimAction::BracketStart, {"Attack"}, false, false},
        {AnimMsg::AttackStop, AnimAction::BracketStop, {}, false, false},
    };
    return t;
}

}
