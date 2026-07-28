#pragma once

#include <algorithm>
#include <cstdint>
#include <limits>
#include <queue>
#include <vector>

#include "sc2/sim/fixed.hpp"

namespace sc2::sim {

struct Waypoint {
    Fixed x, y;
};

struct NavCell {
    int x = 0, y = 0;
};

class NavGrid {
public:
    void configure(int w, int h, Fixed cell_size, Fixed origin_x, Fixed origin_y) {
        w_ = w;
        h_ = h;
        cell_ = cell_size;
        ox_ = origin_x;
        oy_ = origin_y;
        blocked_.assign(size_t(w) * size_t(h), 0);
        blocked_count_ = 0;
    }

    int width() const { return w_; }
    int height() const { return h_; }
    bool configured() const { return w_ > 0 && h_ > 0; }

    bool has_blocked() const { return blocked_count_ > 0; }

    bool in_bounds(int cx, int cy) const {
        return cx >= 0 && cy >= 0 && cx < w_ && cy < h_;
    }

    void to_cell(Fixed wx, Fixed wy, int& cx, int& cy) const {
        cx = ((wx - ox_) / cell_).floor();
        cy = ((wy - oy_) / cell_).floor();
    }

    void cell_center(int cx, int cy, Fixed& wx, Fixed& wy) const {
        Fixed half = cell_ / Fixed::from_int(2);
        wx = ox_ + Fixed::from_int(cx) * cell_ + half;
        wy = oy_ + Fixed::from_int(cy) * cell_ + half;
    }

    bool is_blocked_cell(int cx, int cy) const {
        return in_bounds(cx, cy) && blocked_[idx(cx, cy)] != 0;
    }
    bool is_blocked(Fixed wx, Fixed wy) const {
        int cx, cy;
        to_cell(wx, wy, cx, cy);
        return is_blocked_cell(cx, cy);
    }

    void set_blocked(int cx, int cy, bool b) {
        if (!in_bounds(cx, cy)) return;
        uint8_t& c = blocked_[idx(cx, cy)];
        if (b && !c) {
            c = 1;
            ++blocked_count_;
        } else if (!b && c) {
            c = 0;
            --blocked_count_;
        }
    }
    void block_cell(int cx, int cy) { set_blocked(cx, cy, true); }

    void block_world(Fixed wx, Fixed wy) {
        int cx, cy;
        to_cell(wx, wy, cx, cy);
        set_blocked(cx, cy, true);
    }

    void block_world_rect(Fixed x0, Fixed y0, Fixed x1, Fixed y1) {
        int cx0, cy0, cx1, cy1;
        to_cell(x0, y0, cx0, cy0);
        to_cell(x1, y1, cx1, cy1);
        if (cx1 < cx0) std::swap(cx0, cx1);
        if (cy1 < cy0) std::swap(cy0, cy1);
        for (int y = cy0; y <= cy1; ++y)
            for (int x = cx0; x <= cx1; ++x) set_blocked(x, y, true);
    }

    uint64_t checksum() const {
        uint64_t h = 1469598103934665603ull;
        for (uint8_t c : blocked_) {
            h ^= c;
            h *= 1099511628211ull;
        }
        return h;
    }

private:
    size_t idx(int cx, int cy) const {
        return size_t(cy) * size_t(w_) + size_t(cx);
    }

    int w_ = 0, h_ = 0;
    Fixed cell_ = Fixed::from_int(1);
    Fixed ox_, oy_;
    std::vector<uint8_t> blocked_;
    int blocked_count_ = 0;
};

inline void find_path(const NavGrid& g, int sx, int sy, int gx, int gy,
                      std::vector<NavCell>& out) {
    out.clear();
    if (!g.in_bounds(sx, sy) || !g.in_bounds(gx, gy)) return;
    if (g.is_blocked_cell(sx, sy) || g.is_blocked_cell(gx, gy)) return;

    const int W = g.width();
    const int N = W * g.height();
    const int32_t kInf = std::numeric_limits<int32_t>::max();
    std::vector<int32_t> gscore(size_t(N), kInf);
    std::vector<int32_t> came(size_t(N), -1);
    std::vector<uint8_t> closed(size_t(N), 0);

    auto index = [W](int x, int y) { return y * W + x; };
    auto heur = [gx, gy](int x, int y) -> int32_t {
        int dx = x > gx ? x - gx : gx - x;
        int dy = y > gy ? y - gy : gy - y;
        int mn = dx < dy ? dx : dy;
        int mx = dx < dy ? dy : dx;
        return int32_t(10 * mx + 4 * mn);
    };

    struct QN {
        int32_t f, h;
        int idx;
    };
    struct Worse {
        bool operator()(const QN& a, const QN& b) const {
            if (a.f != b.f) return a.f > b.f;
            if (a.h != b.h) return a.h > b.h;
            return a.idx > b.idx;
        }
    };
    std::priority_queue<QN, std::vector<QN>, Worse> open;

    const int goal = index(gx, gy);
    gscore[size_t(index(sx, sy))] = 0;
    {
        int32_t hs = heur(sx, sy);
        open.push(QN{hs, hs, index(sx, sy)});
    }

    static const int DX[8] = {-1, 0, 1, -1, 1, -1, 0, 1};
    static const int DY[8] = {-1, -1, -1, 0, 0, 1, 1, 1};

    bool found = false;
    while (!open.empty()) {
        const QN cur = open.top();
        open.pop();
        if (closed[size_t(cur.idx)]) continue;
        closed[size_t(cur.idx)] = 1;
        if (cur.idx == goal) {
            found = true;
            break;
        }
        const int cx = cur.idx % W, cy = cur.idx / W;
        for (int k = 0; k < 8; ++k) {
            const int nx = cx + DX[k], ny = cy + DY[k];
            if (!g.in_bounds(nx, ny) || g.is_blocked_cell(nx, ny)) continue;
            const bool diag = (DX[k] != 0 && DY[k] != 0);
            if (diag) {
                if (g.is_blocked_cell(cx + DX[k], cy) ||
                    g.is_blocked_cell(cx, cy + DY[k]))
                    continue;
            }
            const int ni = index(nx, ny);
            if (closed[size_t(ni)]) continue;
            const int32_t step_cost = diag ? 14 : 10;
            const int32_t tentative = gscore[size_t(cur.idx)] + step_cost;
            if (tentative < gscore[size_t(ni)]) {
                gscore[size_t(ni)] = tentative;
                came[size_t(ni)] = cur.idx;
                const int32_t hh = heur(nx, ny);
                open.push(QN{tentative + hh, hh, ni});
            }
        }
    }

    if (!found) return;
    std::vector<NavCell> rev;
    for (int at = goal; at != -1; at = came[size_t(at)])
        rev.push_back(NavCell{at % W, at / W});
    for (size_t i = rev.size(); i-- > 0;) out.push_back(rev[i]);
}

}
