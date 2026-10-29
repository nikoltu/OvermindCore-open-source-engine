#pragma once

#include <cstdint>
#include <functional>
#include <vector>

#include "omc/render/mat4.hpp"
#include "omc/sim/fixed.hpp"
#include "omc/sim/world.hpp"

namespace omc::app {

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

struct PickVolume {
    std::uint32_t shape = 0;
    bool tight = false;
    render::Mat4 inv;
    float size[3] = {0, 0, 0};
};
struct PickVolumes {
    virtual ~PickVolumes() = default;
    virtual bool volumes(std::uint32_t id, std::vector<PickVolume>& out) const = 0;
    virtual bool may_hit(std::uint32_t  , const float*  , const float*  ) const { return true; }
};

bool invert_mat4(const render::Mat4& m, render::Mat4& out);
float ray_volume_t(const PickVolume& v, const float o[3], const float d[3]);

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

    struct RayHit {
        std::uint32_t id = sim::kNoTarget;
        float t = 0.0f;
        bool tight = false;
        float tight_t = 0.0f;
        bool fuzzy = false;
        float fuzzy_t = 0.0f;
    };
    std::vector<RayHit> ray_hits(const sim::World& world, const ScreenPoint& p) const;
    enum class ClickRule { Nearest, TightFirst, OwnFirst, TightOnly, FuzzyOnly };
    std::uint32_t click_pick(const sim::World& world, const ScreenPoint& p, ClickRule rule = ClickRule::TightFirst) const;
    bool has_pick_volumes() const { return volumes_ != nullptr; }
    bool body_has_volumes(std::uint32_t id) const {
        std::vector<PickVolume> v;
        return volumes_ && volumes_->volumes(id, v);
    }

    bool view_ray(const ScreenPoint& p, float o[3], float d[3]) const;
    void set_pick_volumes(const PickVolumes* v) { volumes_ = v; }

    static constexpr std::size_t kSelectionCap = 500;
    struct BoxRules {
        bool on = false;
        std::function<int(const sim::Unit&)> rank;
    };
    void set_box_rules(BoxRules r) { box_rules_ = std::move(r); }
    const BoxRules& box_rules() const { return box_rules_; }
    bool volume_screen_rect(std::uint32_t id, ScreenRect& r, int which = 1) const;

private:
    std::vector<uint32_t> box_select_672(const sim::World& world, const ScreenRect& rect) const;
    BoxRules box_rules_;
    render::Mat4 mvp_;
    render::Mat4 inv_mvp_;
    bool invertible_ = false;
    uint32_t view_w_ = 0, view_h_ = 0;
    int32_t player_ = 0;
    sim::Fixed pick_radius_ = sim::Fixed::from_ratio(1, 2);
    const GroundHeight* ground_ = nullptr;
    const PickVolumes* volumes_ = nullptr;

};

}
