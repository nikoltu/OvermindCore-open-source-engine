#pragma once

#include <cstdint>
#include <vector>

#include "sc2/sim/world.hpp"

namespace sc2::sim {

class TransportBay {
public:
    explicit TransportBay(int32_t capacity = 8) : capacity_(capacity) {}

    int32_t capacity() const { return capacity_; }
    int32_t used() const { return used_; }
    int32_t free() const { return capacity_ - used_; }
    const std::vector<std::uint32_t>& passengers() const { return cargo_; }
    bool contains(std::uint32_t id) const {
        for (std::uint32_t p : cargo_)
            if (p == id) return true;
        return false;
    }

    bool load(World& world, std::uint32_t passenger, int32_t cargo_size = 1) {
        if (passenger >= world.units().size() || cargo_size <= 0) return false;
        if (free() < cargo_size || contains(passenger)) return false;
        Unit& u = world.unit(passenger);
        if (!u.alive || u.loaded) return false;
        u.loaded = true;
        u.orders.clear();
        cargo_.push_back(passenger);
        sizes_.push_back(cargo_size);
        used_ += cargo_size;
        return true;
    }

    bool unload(World& world, std::uint32_t passenger, Fixed dx, Fixed dy) {
        for (std::size_t i = 0; i < cargo_.size(); ++i) {
            if (cargo_[i] != passenger) continue;
            if (passenger < world.units().size()) {
                Unit& u = world.unit(passenger);
                u.loaded = false;
                u.x = dx;
                u.y = dy;
            }
            used_ -= sizes_[i];
            cargo_.erase(cargo_.begin() + std::ptrdiff_t(i));
            sizes_.erase(sizes_.begin() + std::ptrdiff_t(i));
            return true;
        }
        return false;
    }

    void unload_all(World& world, Fixed dx, Fixed dy) {
        while (!cargo_.empty()) unload(world, cargo_.back(), dx, dy);
    }

private:
    int32_t capacity_;
    int32_t used_ = 0;
    std::vector<std::uint32_t> cargo_;
    std::vector<int32_t> sizes_;
};

inline bool set_burrowed(World& world, std::uint32_t id, bool burrow) {
    if (id >= world.units().size()) return false;
    Unit& u = world.unit(id);
    u.burrowed = burrow;
    u.cloaked = burrow;
    if (burrow) u.orders.clear();
    return burrow;
}

}
