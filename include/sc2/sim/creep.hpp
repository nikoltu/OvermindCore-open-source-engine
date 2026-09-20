#pragma once

#include <cstdint>
#include <vector>

#include "sc2/sim/fixed.hpp"

namespace sc2::sim {

class CreepGrid {
public:
    void configure(int w, int h, Fixed cell_size, Fixed origin_x, Fixed origin_y) {
        w_ = w;
        h_ = h;
        cell_ = cell_size;
        ox_ = origin_x;
        oy_ = origin_y;
        cover_.assign(size_t(w) * h, 0);
    }

    int width() const { return w_; }
    int height() const { return h_; }
    Fixed cell_size() const { return cell_; }
    Fixed origin_x() const { return ox_; }
    Fixed origin_y() const { return oy_; }

    bool in_bounds(int cx, int cy) const {
        return cx >= 0 && cy >= 0 && cx < w_ && cy < h_;
    }
    void to_cell(Fixed wx, Fixed wy, int& cx, int& cy) const {
        cx = ((wx - ox_) / cell_).floor();
        cy = ((wy - oy_) / cell_).floor();
    }

    bool is_creep_cell(int cx, int cy) const {
        return in_bounds(cx, cy) && cover_[size_t(cy) * w_ + cx] > 0;
    }
    bool creep_near(Fixed wx, Fixed wy, Fixed r) const {
        int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
        to_cell(wx - r, wy - r, x0, y0);
        to_cell(wx + r, wy + r, x1, y1);
        for (int cy = y0; cy <= y1; ++cy)
            for (int cx = x0; cx <= x1; ++cx) {
                if (!is_creep_cell(cx, cy)) continue;
                const Fixed lx = ox_ + cell_ * Fixed::from_int(cx), ly = oy_ + cell_ * Fixed::from_int(cy);
                const Fixed nx = wx < lx ? lx : (wx > lx + cell_ ? lx + cell_ : wx);
                const Fixed ny = wy < ly ? ly : (wy > ly + cell_ ? ly + cell_ : wy);
                const Fixed dx = wx - nx, dy = wy - ny;
                if (dx * dx + dy * dy <= r * r) return true;
            }
        return false;
    }
    bool is_creep(Fixed wx, Fixed wy) const {
        int cx, cy;
        to_cell(wx, wy, cx, cy);
        return is_creep_cell(cx, cy);
    }

    void stamp_disc(int cx, int cy, int r, int delta) {
        for (int dy = -r; dy <= r; ++dy) {
            for (int dx = -r; dx <= r; ++dx) {
                if (dx * dx + dy * dy > r * r) continue;
                int x = cx + dx, y = cy + dy;
                if (!in_bounds(x, y)) continue;
                uint8_t& c = cover_[size_t(y) * w_ + x];
                if (delta > 0)
                    c = uint8_t(c + 1);
                else if (c > 0)
                    c = uint8_t(c - 1);
            }
        }
    }

    void stamp_mask_upto(int cx, int cy, const std::vector<std::pair<int, int>>& mask, int r, int delta) {
        const long r2 = long(r) * r;
        for (const auto& off : mask) {
            const long dx = off.first, dy = off.second;
            if (dx * dx + dy * dy > r2) continue;
            const int x = cx + off.first, y = cy + off.second;
            if (!in_bounds(x, y)) continue;
            uint8_t& c = cover_[size_t(y) * w_ + x];
            if (delta > 0)
                c = uint8_t(c + 1);
            else if (c > 0)
                c = uint8_t(c - 1);
        }
    }

    uint64_t checksum() const {
        uint64_t h = 1469598103934665603ull;
        for (uint8_t c : cover_) {
            h ^= c;
            h *= 1099511628211ull;
        }
        return h;
    }

private:
    int w_ = 0, h_ = 0;
    Fixed cell_ = Fixed::from_int(1);
    Fixed ox_, oy_;
    std::vector<uint8_t> cover_;
};

inline int mask_growth_radius(const std::vector<std::pair<int, int>>& mask) {
    long worst = 0;
    for (const auto& o : mask) {
        const long d = long(o.first) * o.first + long(o.second) * o.second;
        if (d > worst) worst = d;
    }
    int r = 0;
    while (long(r) * r < worst) ++r;
    return r;
}

struct CreepSource {
    int cx = 0, cy = 0;
    int radius = 1;
    int max_radius = 10;
    int period_ticks = 13;
    int timer = 0;
    bool alive = true;
    std::vector<std::pair<int, int>> mask;
};

class CreepSystem {
public:
    void configure(int w, int h, Fixed cell_size, Fixed origin_x, Fixed origin_y) {
        grid_.configure(w, h, cell_size, origin_x, origin_y);
    }

    int add_source(Fixed wx, Fixed wy, int max_radius, int period_ticks) {
        return add_source(wx, wy, max_radius, period_ticks, {});
    }
    int add_source(Fixed wx, Fixed wy, int max_radius, int period_ticks,
                   std::vector<std::pair<int, int>> mask, int start_radius = 1) {
        CreepSource s;
        grid_.to_cell(wx, wy, s.cx, s.cy);
        s.max_radius = mask.empty() ? max_radius : mask_growth_radius(mask);
        s.period_ticks = period_ticks;
        s.timer = period_ticks;
        s.radius = start_radius < 1 ? 1 : (start_radius > s.max_radius ? s.max_radius : start_radius);
        s.mask = std::move(mask);
        stamp(s, s.radius, +1);
        sources_.push_back(std::move(s));
        return int(sources_.size()) - 1;
    }

    void tick() {
        for (CreepSource& s : sources_) {
            if (!s.alive) {
                if (s.radius > 0 && --s.timer <= 0) {
                    s.timer = s.period_ticks;
                    stamp(s, s.radius, -1);
                    stamp(s, --s.radius, +1);
                    if (s.radius == 0) stamp(s, 0, -1);
                }
                continue;
            }
            if (s.radius >= s.max_radius) continue;
            if (--s.timer > 0) continue;
            s.timer = s.period_ticks;
            stamp(s, s.radius, -1);
            stamp(s, ++s.radius, +1);
        }
    }

    void kill_source(int i) {
        if (i < 0 || i >= int(sources_.size())) return;
        sources_[i].alive = false;
        sources_[i].timer = sources_[i].period_ticks;
    }

    bool is_creep(Fixed wx, Fixed wy) const { return grid_.is_creep(wx, wy); }
    bool has_sources() const { return grid_.width() > 0 && !sources_.empty(); }
    bool creep_near(Fixed wx, Fixed wy, Fixed r) const { return grid_.creep_near(wx, wy, r); }
    const CreepGrid& grid() const { return grid_; }
    uint64_t checksum() const { return grid_.checksum(); }

private:
    void stamp(const CreepSource& s, int r, int delta) {
        if (s.mask.empty())
            grid_.stamp_disc(s.cx, s.cy, r, delta);
        else
            grid_.stamp_mask_upto(s.cx, s.cy, s.mask, r, delta);
    }

    CreepGrid grid_;
    std::vector<CreepSource> sources_;
};

}
