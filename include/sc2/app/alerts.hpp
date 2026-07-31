#pragma once

#include <vector>

#include "sc2/app/hud_messages.hpp"
#include "sc2/sim/fixed.hpp"
#include "sc2/sim/world.hpp"

namespace sc2::app {

class AlertWatcher {
public:
    explicit AlertWatcher(int player = 0) : player_(player) {}

    void set_player(int p) { player_ = p; }
    void set_region_size(sim::Fixed r) { region_ = r; }
    void set_cooldown(float seconds) { cooldown_ = seconds; }

    void observe(const sim::World& world, MessageLog& log, float dt) {
        for (RegionCD& r : regions_) r.cd -= dt;

        const std::vector<sim::Unit>& units = world.units();
        if (shadows_.size() < units.size()) shadows_.resize(units.size());

        for (std::size_t i = 0; i < units.size(); ++i) {
            const sim::Unit& u = units[i];
            Shadow& s = shadows_[i];
            if (u.player != player_ || !s.known) {
                s = Shadow{true, u.alive, u.hp, u.x, u.y};
                continue;
            }
            if (u.alive && u.hp < s.hp) {
                const int cx = (u.x / region_).floor();
                const int cy = (u.y / region_).floor();
                if (region_ready(cx, cy)) {
                    log.emit(MsgZone::Alert, "Your forces are under attack", 6.0f, true,
                             float(u.x.to_double()), float(u.y.to_double()));
                    arm_region(cx, cy);
                }
            }
            if (s.alive && !u.alive)
                log.emit(MsgZone::Alert, "Unit lost", 5.0f, true,
                         float(s.x.to_double()), float(s.y.to_double()));

            s = Shadow{true, u.alive, u.hp, u.x, u.y};
        }
    }

private:
    struct Shadow {
        bool known = false;
        bool alive = true;
        sim::Fixed hp, x, y;
    };
    struct RegionCD {
        int cx, cy;
        float cd;
    };

    bool region_ready(int cx, int cy) const {
        for (const RegionCD& r : regions_)
            if (r.cx == cx && r.cy == cy) return r.cd <= 0.0f;
        return true;
    }
    void arm_region(int cx, int cy) {
        for (RegionCD& r : regions_)
            if (r.cx == cx && r.cy == cy) {
                r.cd = cooldown_;
                return;
            }
        regions_.push_back({cx, cy, cooldown_});
    }

    int player_;
    std::vector<Shadow> shadows_;
    std::vector<RegionCD> regions_;
    float cooldown_ = 5.0f;
    sim::Fixed region_ = sim::Fixed::from_int(10);
};

}
