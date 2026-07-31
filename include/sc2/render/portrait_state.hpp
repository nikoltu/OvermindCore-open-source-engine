#pragma once

namespace sc2::render {

enum class PortraitState { Idle, Talk, Wounded, Death, Taunt };

class PortraitController {
public:
    float wounded_below = 0.33f;

    PortraitState state() const { return state_; }

    void trigger_talk(float seconds) {
        if (state_ == PortraitState::Death) return;
        talk_left_ = seconds;
    }
    void trigger_taunt(float seconds) {
        if (state_ == PortraitState::Death) return;
        taunt_left_ = seconds;
    }
    void kill() { dead_ = true; }

    void update(float life_frac, float dt) {
        if (talk_left_ > 0) talk_left_ -= dt;
        if (taunt_left_ > 0) taunt_left_ -= dt;

        if (dead_) {
            state_ = PortraitState::Death;
            return;
        }
        if (talk_left_ > 0) {
            state_ = PortraitState::Talk;
            return;
        }
        if (taunt_left_ > 0) {
            state_ = PortraitState::Taunt;
            return;
        }
        state_ = (life_frac < wounded_below) ? PortraitState::Wounded : PortraitState::Idle;
    }

private:
    PortraitState state_ = PortraitState::Idle;
    float talk_left_ = 0.0f;
    float taunt_left_ = 0.0f;
    bool dead_ = false;
};

}
