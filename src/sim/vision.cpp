#include "sc2/sim/vision.hpp"

#include "sc2/sim/terrain_map.hpp"
#include "sc2/sim/world.hpp"

namespace sc2::sim {

void VisionGrid::reconfigure(Fixed cell_size, const Bounds& bounds) {
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
    cells_.assign(size_t(w_) * size_t(h_), Visibility::Hidden);
}

void VisionGrid::compute(const World& world, int32_t player, Fixed sight_radius,
                         Fixed cell_size, const Bounds& bounds, const TerrainMap* terrain) {
    reconfigure(cell_size, bounds);
    if (cells_.empty()) return;

    for (Visibility& v : cells_)
        if (v == Visibility::Visible) v = Visibility::Explored;

    if (sight_radius.raw() <= 0) return;

    const Fixed half = Fixed::from_raw(cell_.raw() / 2);

    for (const Unit& u : world.units()) {
        if (!u.alive || u.player != player) continue;

        const Fixed sr = u.sight.raw() > 0 ? u.sight : sight_radius;
        const Fixed reach = sr + half;
        const int64_t r2 = int64_t(reach.raw()) * int64_t(reach.raw());
        const Fixed eye_x = bounds_.min_x + cell_ * Fixed::from_int(((u.x - bounds_.min_x) / cell_).floor()) + half;
        const Fixed eye_y = bounds_.min_y + cell_ * Fixed::from_int(((u.y - bounds_.min_y) / cell_).floor()) + half;

        constexpr Fixed kOverCliffVisionHeight = Fixed::from_int(15);
        const bool sees_over_cliffs =
            u.flying || u.vision_height.raw() >= kOverCliffVisionHeight.raw();
        const std::uint8_t viewer_level =
            sees_over_cliffs ? std::uint8_t(0xFF) : (terrain ? terrain->level_at(u.x, u.y) : 0);

        int ix0 = ((eye_x - reach - bounds_.min_x) / cell_).floor();
        int ix1 = ((eye_x + reach - bounds_.min_x) / cell_).floor();
        int iy0 = ((eye_y - reach - bounds_.min_y) / cell_).floor();
        int iy1 = ((eye_y + reach - bounds_.min_y) / cell_).floor();
        if (ix0 < 0) ix0 = 0;
        if (iy0 < 0) iy0 = 0;
        if (ix1 >= w_) ix1 = w_ - 1;
        if (iy1 >= h_) iy1 = h_ - 1;

        for (int cy = iy0; cy <= iy1; ++cy) {
            Fixed centre_y = bounds_.min_y + cell_ * Fixed::from_int(cy) + half;
            int64_t dy = int64_t((centre_y - eye_y).raw());
            for (int cx = ix0; cx <= ix1; ++cx) {
                Fixed centre_x = bounds_.min_x + cell_ * Fixed::from_int(cx) + half;
                int64_t dx = int64_t((centre_x - eye_x).raw());
                if (dx * dx + dy * dy > r2) continue;
                if (terrain) {
                    int tx, ty;
                    terrain->to_cell(centre_x, centre_y, tx, ty);
                    if (!terrain->vision_reaches(viewer_level, tx, ty)) continue;
                }
                cells_[size_t(cy) * size_t(w_) + size_t(cx)] = Visibility::Visible;
            }
        }
    }
}

}
