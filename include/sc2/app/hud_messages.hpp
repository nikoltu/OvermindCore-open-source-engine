#pragma once

#include <algorithm>
#include <string>
#include <vector>

namespace sc2::app {

enum class MsgZone {
    Error,
    Warning,
    Transmission,
    Alert,
};

struct HudMessage {
    MsgZone zone;
    std::string text;
    float ttl = 0;
    float ttl0 = 0;
    int count = 1;
    bool has_target = false;
    float tx = 0, ty = 0;
    std::string icon;
};

class MessageLog {
public:
    void emit(MsgZone zone, const std::string& text, float ttl = 5.0f,
              bool has_target = false, float tx = 0, float ty = 0) {
        for (HudMessage& m : msgs_) {
            if (m.zone == zone && m.text == text) {
                m.ttl = std::max(m.ttl, ttl);
                m.ttl0 = m.ttl;
                ++m.count;
                m.has_target = has_target;
                m.tx = tx;
                m.ty = ty;
                return;
            }
        }
        msgs_.push_back({zone, text, ttl, ttl, 1, has_target, tx, ty, std::string()});
    }

    void advance(float dt) {
        for (HudMessage& m : msgs_) m.ttl -= dt;
        msgs_.erase(std::remove_if(msgs_.begin(), msgs_.end(),
                                   [](const HudMessage& m) { return m.ttl <= 0.0f; }),
                    msgs_.end());
    }

    std::vector<const HudMessage*> zone(MsgZone z) const {
        std::vector<const HudMessage*> out;
        for (const HudMessage& m : msgs_)
            if (m.zone == z) out.push_back(&m);
        return out;
    }

    const HudMessage* latest_alert_target() const {
        const HudMessage* best = nullptr;
        for (const HudMessage& m : msgs_)
            if (m.zone == MsgZone::Alert && m.has_target) best = &m;
        return best;
    }

    const std::vector<HudMessage>& all() const { return msgs_; }
    std::vector<HudMessage>& mutable_all() { return msgs_; }
    std::size_t size() const { return msgs_.size(); }
    void clear() { msgs_.clear(); }

private:
    std::vector<HudMessage> msgs_;
};

}
