#include "sc2/sim/detection.hpp"

#include "sc2/sim/world.hpp"

namespace sc2::sim {

void DetectionGrid::reconfigure(Fixed cell_size, const Bounds& bounds) {
    const bool same = !cells_.empty() && cell_ == cell_size &&
                      bounds_.min_x == bounds.min_x && bounds_.min_y == bounds.min_y &&
                      bounds_.max_x == bounds.max_x && bounds_.max_y == bounds.max_y &&
                      cells_.size() == size_t(w_) * size_t(h_);
    if (same) return;

    cell_ = cell_size;
    bounds_ = bounds;
    auto cell_count = [&](Fixed span) -> int {
        if (span.raw() <= 0 || cell_size.raw() <= 0) return 0;
        int n = (span / cell_size).floor();
        if (cell_size * Fixed::from_int(n) < span) ++n;
        return n;
    };
    w_ = cell_count(bounds.max_x - bounds.min_x);
    h_ = cell_count(bounds.max_y - bounds.min_y);
    cells_.assign(size_t(w_) * size_t(h_), 0);
}

void DetectionGrid::compute(const World& world, int32_t player, Fixed detect_radius,
                            Fixed cell_size, const Bounds& bounds) {
    reconfigure(cell_size, bounds);
    if (cells_.empty()) return;

    for (std::uint8_t& c : cells_) c = 0;
    if (detect_radius.raw() <= 0) return;

    const Fixed half = Fixed::from_raw(cell_.raw() / 2);

    for (const Unit& u : world.units()) {
        if (!u.alive || u.player != player || !u.detector) continue;

        const Fixed dr = u.detect_range.raw() > 0 ? u.detect_range : detect_radius;
        const int64_t r2 = int64_t(dr.raw()) * int64_t(dr.raw());

        int ix0 = ((u.x - dr - bounds_.min_x) / cell_).floor();
        int ix1 = ((u.x + dr - bounds_.min_x) / cell_).floor();
        int iy0 = ((u.y - dr - bounds_.min_y) / cell_).floor();
        int iy1 = ((u.y + dr - bounds_.min_y) / cell_).floor();
        if (ix0 < 0) ix0 = 0;
        if (iy0 < 0) iy0 = 0;
        if (ix1 >= w_) ix1 = w_ - 1;
        if (iy1 >= h_) iy1 = h_ - 1;

        for (int cy = iy0; cy <= iy1; ++cy) {
            Fixed centre_y = bounds_.min_y + cell_ * Fixed::from_int(cy) + half;
            int64_t dy = int64_t((centre_y - u.y).raw());
            for (int cx = ix0; cx <= ix1; ++cx) {
                Fixed centre_x = bounds_.min_x + cell_ * Fixed::from_int(cx) + half;
                int64_t dx = int64_t((centre_x - u.x).raw());
                if (dx * dx + dy * dy <= r2)
                    cells_[size_t(cy) * size_t(w_) + size_t(cx)] = 1;
            }
        }
    }
}

bool is_revealed(int32_t viewer_player, const Unit& target, const VisionGrid& vision,
                 const DetectionGrid& detection) {
    (void)viewer_player;
    if (!vision.is_visible(target.x, target.y)) return false;
    if (!target.cloaked) return true;
    return detection.covers_world(target.x, target.y);
}

}
