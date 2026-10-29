#pragma once

#include <cstdint>
#include <vector>

#include "omc/sim/fixed.hpp"

namespace omc::sim {

class World;
class TerrainMap;

enum class Visibility : uint8_t {
    Hidden = 0,
    Explored = 1,
    Visible = 2,
};

struct Bounds {
    Fixed min_x, min_y, max_x, max_y;
};

class VisionGrid {
public:
    void compute(const World& world, int32_t player, Fixed sight_radius,
                 Fixed cell_size, const Bounds& bounds, const TerrainMap* terrain = nullptr);

    int width() const { return w_; }
    int height() const { return h_; }

    Visibility state(int cx, int cy) const {
        if (cx < 0 || cy < 0 || cx >= w_ || cy >= h_) return Visibility::Hidden;
        return cells_[size_t(cy) * size_t(w_) + size_t(cx)];
    }

    Visibility state_at(Fixed wx, Fixed wy) const {
        int cx, cy;
        to_cell(wx, wy, cx, cy);
        return state(cx, cy);
    }

    bool is_visible(Fixed wx, Fixed wy) const {
        return state_at(wx, wy) == Visibility::Visible;
    }
    bool is_explored(Fixed wx, Fixed wy) const {
        return state_at(wx, wy) != Visibility::Hidden;
    }

    void to_cell(Fixed wx, Fixed wy, int& cx, int& cy) const {
        if (cell_.raw() <= 0) {
            cx = cy = -1;
            return;
        }
        cx = ((wx - bounds_.min_x) / cell_).floor();
        cy = ((wy - bounds_.min_y) / cell_).floor();
    }

    uint64_t checksum() const {
        uint64_t h = 1469598103934665603ull;
        for (Visibility v : cells_) {
            h ^= uint8_t(v);
            h *= 1099511628211ull;
        }
        return h;
    }

private:
    void reconfigure(Fixed cell_size, const Bounds& bounds);

    int w_ = 0, h_ = 0;
    Fixed cell_;
    Bounds bounds_{};
    std::vector<Visibility> cells_;
};

}
