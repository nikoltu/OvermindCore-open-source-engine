#pragma once

#include <cstdint>
#include <vector>

#include "sc2/render/mat4.hpp"
#include "sc2/sim/fixed.hpp"
#include "sc2/sim/world.hpp"

namespace sc2::app {

struct ScreenRect {
    float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
};

struct ScreenPoint {
    float x = 0, y = 0;
};

struct GroundHeight {
    virtual ~GroundHeight() = default;
    virtual float z_at(float wx, float wy) const = 0;
    virtual float max_z() const = 0;
};

enum class CommandKind {
    None,
    Move,
    AttackMove,
    Attack,
    Gather,
    Follow,
};

struct Command {
    CommandKind kind = CommandKind::None;
    sim::Fixed x, y;
    uint32_t target = sim::kNoTarget;
};

struct FormationTarget {
    uint32_t id = sim::kNoTarget;
    sim::Fixed x, y;
};
std::vector<FormationTarget> formation_targets(const sim::World& world, const std::vector<uint32_t>& selection,
                                               sim::Fixed tx, sim::Fixed ty, float mixed_formation_diameter = 6.0f,
                                               float air_formation_diameter = 8.0f);

class Control {
public:
    Control(const render::Mat4& mvp, uint32_t view_w, uint32_t view_h,
            int32_t player);

    std::vector<uint32_t> box_select(const sim::World& world, const ScreenRect& rect,
                                     bool marquee = false) const;

    void set_ground_height(const GroundHeight* g) { ground_ = g; }
    const GroundHeight* ground_height() const { return ground_; }

    bool ground_point(const sim::World& world, const ScreenPoint& p, sim::Fixed& wx,
                      sim::Fixed& wy) const;

    uint32_t pick_target(const sim::World& world, const ScreenPoint& p) const;

    uint32_t pick_own(const sim::World& world, const ScreenPoint& p) const;
    uint32_t pick_ally(const sim::World& world, const ScreenPoint& p) const;

    uint32_t pick_resource(const sim::World& world, const ScreenPoint& p) const;

    Command resolve_command(const sim::World& world, const ScreenPoint& p,
                            bool attack_move = false) const;

    Command issue_command(sim::World& world, const std::vector<uint32_t>& selection,
                          const ScreenPoint& p, bool attack_move = false,
                          bool queued = false) const;

    sim::Fixed pick_radius() const { return pick_radius_; }

    bool project(float x, float y, float z, float& px, float& py) const;

    bool project_on_ground(float x, float y, float& px, float& py) const {
        return project(x, y, ground_z(x, y), px, py);
    }

    float ground_z(float x, float y) const { return ground_ ? ground_->z_at(x, y) : 0.0f; }

private:
    render::Mat4 mvp_;
    render::Mat4 inv_mvp_;
    bool invertible_ = false;
    uint32_t view_w_ = 0, view_h_ = 0;
    int32_t player_ = 0;
    sim::Fixed pick_radius_ = sim::Fixed::from_ratio(1, 2);
    const GroundHeight* ground_ = nullptr;

};

}
