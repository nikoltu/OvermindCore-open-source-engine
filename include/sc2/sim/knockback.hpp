#pragma once

#include <cstdint>
#include <vector>

#include "sc2/sim/fixed.hpp"
#include "sc2/sim/world.hpp"

namespace sc2::sim {

class KnockbackSystem {
public:
    void push(const World& world, std::uint32_t target, Fixed fx, Fixed fy, Fixed distance,
              Fixed duration) {
        if (target >= world.units().size()) return;
        const Unit& u = world.units()[target];
        add(target, u.x - fx, u.y - fy, distance, duration);
    }
    void pull(const World& world, std::uint32_t target, Fixed tx, Fixed ty, Fixed distance,
              Fixed duration) {
        if (target >= world.units().size()) return;
        const Unit& u = world.units()[target];
        add(target, tx - u.x, ty - u.y, distance, duration);
    }

    int active_count() const {
        int n = 0;
        for (const Impulse& i : impulses_)
            if (i.active) ++n;
        return n;
    }

    void advance(World& world, Fixed dt) {
        const std::uint32_t n = std::uint32_t(world.units().size());
        for (Impulse& im : impulses_) {
            if (!im.active) continue;
            if (im.target >= n || !world.units()[im.target].alive) {
                im.active = false;
                continue;
            }
            Fixed step = im.speed * dt;
            if (step >= im.remaining) step = im.remaining;
            Unit& u = world.unit(im.target);
            u.x = u.x + im.dx * step;
            u.y = u.y + im.dy * step;
            u.path.clear();
            u.path_next = 0;
            u.path_gx = INT32_MIN;
            u.path_gy = INT32_MIN;
            im.remaining = im.remaining - step;
            if (im.remaining.raw() <= 0) im.active = false;
        }
    }

private:
    struct Impulse {
        std::uint32_t target = kNoTarget;
        Fixed dx, dy;
        Fixed speed;
        Fixed remaining;
        bool active = false;
    };

    void add(std::uint32_t target, Fixed vx, Fixed vy, Fixed distance, Fixed duration) {
        if (distance.raw() <= 0 || duration.raw() <= 0) return;
        Fixed len = isqrt(vx * vx + vy * vy);
        Impulse im;
        im.target = target;
        if (len.raw() <= 0) {
            im.dx = Fixed::from_int(1);
            im.dy = Fixed();
        } else {
            im.dx = vx / len;
            im.dy = vy / len;
        }
        im.remaining = distance;
        im.speed = distance / duration;
        im.active = true;
        impulses_.push_back(im);
    }

    std::vector<Impulse> impulses_;
};

}
