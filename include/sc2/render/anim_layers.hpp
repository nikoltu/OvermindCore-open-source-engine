#pragma once

#include <string>
#include <vector>

namespace sc2::render {

class Bracket {
public:
    enum class Phase { Idle, Start, Content, End, Done };

    void begin() {
        phase_ = Phase::Start;
        held_ = true;
    }
    void release() { held_ = false; }
    void on_clip_end() {
        switch (phase_) {
            case Phase::Start: phase_ = Phase::Content; break;
            case Phase::Content: if (!held_) phase_ = Phase::End; break;
            case Phase::End: phase_ = Phase::Done; break;
            default: break;
        }
    }
    void reset() {
        phase_ = Phase::Idle;
        held_ = false;
    }

    Phase phase() const { return phase_; }
    bool active() const {
        return phase_ == Phase::Start || phase_ == Phase::Content || phase_ == Phase::End;
    }
    bool looping() const { return phase_ == Phase::Content && held_; }
    bool held() const { return held_; }

private:
    Phase phase_ = Phase::Idle;
    bool held_ = false;
};

struct BracketTokens {
    std::vector<std::string> start, content, end;
    const std::vector<std::string>* for_phase(Bracket::Phase p) const {
        switch (p) {
            case Bracket::Phase::Start: return start.empty() ? nullptr : &start;
            case Bracket::Phase::Content: return content.empty() ? nullptr : &content;
            case Bracket::Phase::End: return end.empty() ? nullptr : &end;
            default: return nullptr;
        }
    }
};

struct AnimLayerStack {
    std::vector<std::string> baseline;
    BracketTokens bracket_tokens;
    Bracket bracket;
    std::vector<std::string> oneshot;
    bool oneshot_active = false;
    bool oneshot_loops = false;
    bool latched = false;
    std::vector<std::vector<std::string>> additives;

    const std::vector<std::string>& body() const {
        if (oneshot_active) return oneshot;
        if (const std::vector<std::string>* b = bracket_tokens.for_phase(bracket.phase()))
            return *b;
        return baseline;
    }
    bool body_loops() const {
        if (oneshot_active) return oneshot_loops;
        if (bracket_tokens.for_phase(bracket.phase())) return bracket.looping();
        return true;
    }
};

}
