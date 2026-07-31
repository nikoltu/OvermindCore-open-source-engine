#pragma once

#include <cstdint>
#include <vector>

#include "sc2/sim/fixed.hpp"
#include "sc2/sim/vision.hpp"

namespace sc2::sim {

class World;
struct Unit;

class DetectionGrid {
public:
    void compute(const World& world, int32_t player, Fixed detect_radius, Fixed cell_size,
                 const Bounds& bounds);

    int width() const { return w_; }
    int height() const { return h_; }

    bool covers(int cx, int cy) const {
        if (cx < 0 || cy < 0 || cx >= w_ || cy >= h_) return false;
        return cells_[size_t(cy) * size_t(w_) + size_t(cx)] != 0;
    }
    bool covers_world(Fixed wx, Fixed wy) const {
        int cx, cy;
        to_cell(wx, wy, cx, cy);
        return covers(cx, cy);
    }

    void to_cell(Fixed wx, Fixed wy, int& cx, int& cy) const {
        if (cell_.raw() <= 0) {
            cx = cy = -1;
            return;
        }
        cx = ((wx - bounds_.min_x) / cell_).floor();
        cy = ((wy - bounds_.min_y) / cell_).floor();
    }

    std::uint64_t checksum() const {
        std::uint64_t h = 1469598103934665603ull;
        for (std::uint8_t c : cells_) {
            h ^= c;
            h *= 1099511628211ull;
        }
        return h;
    }

private:
    void reconfigure(Fixed cell_size, const Bounds& bounds);

    int w_ = 0, h_ = 0;
    Fixed cell_;
    Bounds bounds_{};
    std::vector<std::uint8_t> cells_;
};

bool is_revealed(int32_t viewer_player, const Unit& target, const VisionGrid& vision,
                 const DetectionGrid& detection);

}
