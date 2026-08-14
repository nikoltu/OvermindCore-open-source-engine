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

enum class CommandKind {
    None,
    Move,
    AttackMove,
    Attack,
    Gather,
};

struct Command {
    CommandKind kind = CommandKind::None;
    sim::Fixed x, y;
    uint32_t target = sim::kNoTarget;
};

class Control {
public:
    Control(const render::Mat4& mvp, uint32_t view_w, uint32_t view_h,
            int32_t player);

    std::vector<uint32_t> box_select(const sim::World& world,
                                     const ScreenRect& rect) const;

    bool ground_point(const sim::World& world, const ScreenPoint& p, sim::Fixed& wx,
                      sim::Fixed& wy) const;

    uint32_t pick_target(const sim::World& world, const ScreenPoint& p) const;

    uint32_t pick_own(const sim::World& world, const ScreenPoint& p) const;

    uint32_t pick_resource(const sim::World& world, const ScreenPoint& p) const;

    Command resolve_command(const sim::World& world, const ScreenPoint& p,
                            bool attack_move = false) const;

    Command issue_command(sim::World& world, const std::vector<uint32_t>& selection,
                          const ScreenPoint& p, bool attack_move = false) const;

    void set_pick_radius(sim::Fixed r) { pick_radius_ = r; }
    sim::Fixed pick_radius() const { return pick_radius_; }

private:
    render::Mat4 mvp_;
    render::Mat4 inv_mvp_;
    bool invertible_ = false;
    uint32_t view_w_ = 0, view_h_ = 0;
    int32_t player_ = 0;
    sim::Fixed pick_radius_ = sim::Fixed::from_ratio(1, 2);

    bool project(float x, float y, float z, float& px, float& py) const;
};

}
