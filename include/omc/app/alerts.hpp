#pragma once

#include <algorithm>
#include <cstdlib>
#include <string>
#include <vector>

#include "omc/app/hud_messages.hpp"
#include "omc/sim/fixed.hpp"
#include "omc/sim/world.hpp"

namespace omc::app {

enum class AlertKind { AttackUnit, AttackTown, UpgradeComplete, BuildComplete, ResourceExhausted };
struct AlertFired {
    AlertKind kind = AlertKind::AttackUnit;
    float x = 0.0f, y = 0.0f;
};

class AlertWatcher {
public:
    explicit AlertWatcher(int player = 0) : player_(player) {}
    const std::vector<AlertFired>& fired() const { return fired_; }

    void set_player(int p) { player_ = p; }

    struct Overlap {
        float duration = 15.0f, local_radius = 15.0f, life = 15.0f;
        int local_count = 1, global_count = 2;
    };
    void set_overlap(const Overlap& o) { overlap_ = o; }

    void observe(const sim::World& world, MessageLog& log, float dt) {
        fired_.clear();
        for (Active& a : active_) a.age += dt;
        active_.erase(std::remove_if(active_.begin(), active_.end(),
                                     [&](const Active& a) { return a.age >= overlap_.duration; }),
                      active_.end());

        const std::vector<sim::Unit>& units = world.units();
        if (shadows_.size() < units.size()) shadows_.resize(units.size());

        for (std::size_t i = 0; i < units.size(); ++i) {
            const sim::Unit& u = units[i];
            Shadow& s = shadows_[i];
            static const bool kMorphOld = std::getenv("OMC_ALERT_MORPH_OLD") != nullptr;
            static const bool kLifeCostOld = std::getenv("OMC_ALERT_LIFE_COST_OLD") != nullptr;
            if (u.player != player_ || !s.known) {
                s = Shadow{true, u.alive, u.hp, u.shields, u.x, u.y, u.catalog_id, u.life_paid};
                continue;
            }
            const bool reshaped = !kMorphOld && (u.catalog_id != s.type || world.is_morphing(std::uint32_t(i)));
            const bool paid = !kLifeCostOld && u.shields >= s.shields && u.hp < s.hp && s.hp - u.hp <= u.life_paid - s.paid;
            if (u.alive && !reshaped && !paid && (u.hp < s.hp || u.shields < s.shields)) {
                const bool authored = !u.catalog_id.empty();
                const AlertKind kind = (authored ? u.town_alert : u.speed.raw() == 0)
                                           ? AlertKind::AttackTown
                                           : AlertKind::AttackUnit;
                const float x = float(u.x.to_double()), y = float(u.y.to_double());
                if (admit(kind, x, y)) {
                    log.emit(MsgZone::Alert, "Your forces are under attack", overlap_.life, true, x, y);
                    active_.push_back({kind, x, y, 0.0f});
                    fired_.push_back({kind, x, y});
                }
            }
            s = Shadow{true, u.alive, u.hp, u.shields, u.x, u.y, u.catalog_id, u.life_paid};
        }
    }

private:
    struct Shadow {
        bool known = false;
        bool alive = true;
        sim::Fixed hp, shields, x, y;
        std::string type;
        sim::Fixed paid;
    };
    struct Active {
        AlertKind kind;
        float x, y, age;
    };
    std::vector<AlertFired> fired_;

    bool admit(AlertKind kind, float x, float y) const {
        int local = 0, global = 0;
        const float r2 = overlap_.local_radius * overlap_.local_radius;
        for (const Active& a : active_) {
            if (a.kind != kind) continue;
            ++global;
            if ((a.x - x) * (a.x - x) + (a.y - y) * (a.y - y) <= r2) ++local;
        }
        return local < overlap_.local_count && global < overlap_.global_count;
    }

    int player_;
    std::vector<Shadow> shadows_;
    std::vector<Active> active_;
    Overlap overlap_;
};

}
