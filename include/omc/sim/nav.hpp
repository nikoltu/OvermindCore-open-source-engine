#pragma once

#include <algorithm>
#include <cstdint>
#include <limits>
#include <queue>
#include <vector>

#include "omc/sim/fixed.hpp"

namespace omc::sim {

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
        levels_.clear();
        ramps_.clear();
        level_count_ = 0;
    }

    int width() const { return w_; }
    Fixed cell_size() const { return cell_; }
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
    Fixed clearance(int cx, int cy) const {
        if (!in_bounds(cx, cy)) return Fixed();
        ensure_clearance();
        return Fixed::from_raw(int32_t(clearance_[idx(cx, cy)]) * (cell_.raw() / 16));
    }
    bool admits(int cx, int cy, Fixed radius) const {
        return radius.raw() <= 0 || clearance(cx, cy) >= radius;
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
            ++revision_;
        } else if (!b && c) {
            c = 0;
            --blocked_count_;
            ++revision_;
        }
    }
    uint32_t revision() const { return revision_; }
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

    bool has_levels() const { return level_count_ > 0; }

    void set_level(int cx, int cy, uint8_t level) {
        if (!in_bounds(cx, cy)) return;
        if (levels_.empty()) levels_.assign(size_t(w_) * size_t(h_), 0);
        uint8_t& c = levels_[idx(cx, cy)];
        if (c == level) return;
        if (c == 0 && level != 0) ++level_count_;
        else if (c != 0 && level == 0) --level_count_;
        c = level;
    }
    uint8_t level_cell(int cx, int cy) const {
        return (!levels_.empty() && in_bounds(cx, cy)) ? levels_[idx(cx, cy)] : uint8_t(0);
    }

    bool can_traverse(int fx, int fy, int tx, int ty, bool can_jump = false,
                      bool can_walk_cliffs = false) const {
        if (!has_levels()) return true;
        if (level_cell(fx, fy) == level_cell(tx, ty)) return true;
        if (is_ramp_cell(fx, fy) || is_ramp_cell(tx, ty)) return true;
        return can_jump || can_walk_cliffs;
    }

    bool is_leap_edge(int fx, int fy, int tx, int ty, bool can_walk_cliffs = false) const {
        if (!has_levels() || can_walk_cliffs) return false;
        if (level_cell(fx, fy) == level_cell(tx, ty)) return false;
        return !(is_ramp_cell(fx, fy) || is_ramp_cell(tx, ty));
    }

    bool has_cliffs() const { return cliff_count_ > 0; }
    void mark_cliff(int cx, int cy, bool c) {
        if (!in_bounds(cx, cy)) return;
        if (cliff_.empty()) {
            if (!c) return;
            cliff_.assign(size_t(w_) * size_t(h_), 0);
        }
        uint8_t& v = cliff_[idx(cx, cy)];
        if (c && !v) { v = 1; ++cliff_count_; }
        else if (!c && v) { v = 0; --cliff_count_; }
    }
    bool is_cliff_cell(int cx, int cy) const {
        return (!cliff_.empty() && in_bounds(cx, cy)) ? cliff_[idx(cx, cy)] != 0 : false;
    }

    bool has_ff() const { return ff_count_ > 0; }
    void mark_ff(int cx, int cy, bool f) {
        if (!in_bounds(cx, cy)) return;
        if (ff_.empty()) {
            if (!f) return;
            ff_.assign(size_t(w_) * size_t(h_), 0);
        }
        uint8_t& v = ff_[idx(cx, cy)];
        if (f && !v) { v = 1; ++ff_count_; }
        else if (!f && v) { v = 0; --ff_count_; }
    }
    bool is_ff_cell(int cx, int cy) const {
        return (!ff_.empty() && in_bounds(cx, cy)) ? ff_[idx(cx, cy)] != 0 : false;
    }

    void set_ramp(int cx, int cy, bool r) {
        if (!in_bounds(cx, cy)) return;
        if (ramps_.empty()) {
            if (!r) return;
            ramps_.assign(size_t(w_) * size_t(h_), 0);
        }
        ramps_[idx(cx, cy)] = r ? 1 : 0;
    }
    bool is_ramp_cell(int cx, int cy) const {
        return (!ramps_.empty() && in_bounds(cx, cy)) ? ramps_[idx(cx, cy)] != 0 : false;
    }

    void mark_ramps_from_levels() {
        if (!has_levels()) return;
        static const int dx[4] = {-1, 1, 0, 0}, dy[4] = {0, 0, -1, 1};
        for (int y = 0; y < h_; ++y)
            for (int x = 0; x < w_; ++x) {
                if (is_blocked_cell(x, y)) continue;
                const uint8_t lv = level_cell(x, y);
                for (int k = 0; k < 4; ++k) {
                    const int nx = x + dx[k], ny = y + dy[k];
                    if (in_bounds(nx, ny) && level_cell(nx, ny) != lv) {
                        set_ramp(x, y, true);
                        break;
                    }
                }
            }
    }

    bool segment_walkable(Fixed ax, Fixed ay, Fixed bx, Fixed by, bool can_walk_cliffs = false, Fixed radius = Fixed()) const {
        int cx, cy, ex, ey;
        to_cell(ax, ay, cx, cy);
        to_cell(bx, by, ex, ey);
        const int64_t dx = int64_t(bx.raw()) - ax.raw(), dy = int64_t(by.raw()) - ay.raw();
        const int sx = dx > 0 ? 1 : (dx < 0 ? -1 : 0), sy = dy > 0 ? 1 : (dy < 0 ? -1 : 0);
        const int64_t adx = dx < 0 ? -dx : dx, ady = dy < 0 ? -dy : dy;
        auto enter = [&](int fx, int fy, int tx, int ty) {
            return in_bounds(tx, ty) && !is_blocked_cell(tx, ty) &&
                   can_traverse(fx, fy, tx, ty, false, can_walk_cliffs) &&
                   (radius.raw() <= 0 || (tx == ex && ty == ey) || admits(tx, ty, radius));
        };
        const int limit = (ex > cx ? ex - cx : cx - ex) + (ey > cy ? ey - cy : cy - ey) + 2;
        for (int it = 0; it < limit && (cx != ex || cy != ey); ++it) {
            const int64_t nbx = int64_t(ox_.raw()) + int64_t(cell_.raw()) * (sx > 0 ? cx + 1 : cx);
            const int64_t nby = int64_t(oy_.raw()) + int64_t(cell_.raw()) * (sy > 0 ? cy + 1 : cy);
            const int64_t distx = sx == 0 ? 0 : (sx > 0 ? nbx - ax.raw() : ax.raw() - nbx);
            const int64_t disty = sy == 0 ? 0 : (sy > 0 ? nby - ay.raw() : ay.raw() - nby);
            int cmp;
            if (sx == 0) cmp = 1;
            else if (sy == 0) cmp = -1;
            else {
                const int64_t lx = distx * ady, ly = disty * adx;
                cmp = lx < ly ? -1 : (lx > ly ? 1 : 0);
            }
            if (cmp < 0) {
                if (!enter(cx, cy, cx + sx, cy)) return false;
                cx += sx;
            } else if (cmp > 0) {
                if (!enter(cx, cy, cx, cy + sy)) return false;
                cy += sy;
            } else {
                if (!enter(cx, cy, cx + sx, cy) || !enter(cx, cy, cx, cy + sy) ||
                    !enter(cx, cy, cx + sx, cy + sy))
                    return false;
                cx += sx;
                cy += sy;
            }
        }
        return cx == ex && cy == ey;
    }

    uint64_t checksum() const {
        uint64_t h = 1469598103934665603ull;
        for (uint8_t c : blocked_) {
            h ^= c;
            h *= 1099511628211ull;
        }
        for (uint8_t c : levels_) {
            h ^= c;
            h *= 1099511628211ull;
        }
        for (uint8_t c : ramps_) {
            h ^= c;
            h *= 1099511628211ull;
        }
        for (uint8_t c : cliff_) {
            h ^= c;
            h *= 1099511628211ull;
        }
        for (uint8_t c : ff_) {
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
    mutable std::vector<uint8_t> clearance_;
    mutable uint32_t clearance_rev_ = 0xFFFFFFFFu;
    void ensure_clearance() const {
        if (clearance_rev_ == revision_ && clearance_.size() == blocked_.size()) return;
        clearance_.assign(blocked_.size(), 0);
        constexpr int kWin = 4;
        for (int y = 0; y < h_; ++y)
            for (int x = 0; x < w_; ++x) {
                if (blocked_[idx(x, y)]) continue;
                int best = (kWin + 1) * (kWin + 1) * 2;
                for (int dy = -kWin; dy <= kWin; ++dy)
                    for (int dx = -kWin; dx <= kWin; ++dx) {
                        const int nx = x + dx, ny = y + dy;
                        const bool wall = !in_bounds(nx, ny) || blocked_[idx(nx, ny)];
                        if (!wall) continue;
                        const int d2 = dx * dx + dy * dy;
                        if (d2 < best) best = d2;
                    }
                int32_t r = 0;
                {
                    const int64_t v = int64_t(best) * 256;
                    int64_t lo = 0, hi = 4096;
                    while (lo < hi) { const int64_t mid = (lo + hi + 1) / 2; if (mid * mid <= v) lo = mid; else hi = mid - 1; }
                    r = int32_t(lo);
                }
                int32_t c = r - 8;
                if (c < 0) c = 0;
                if (c > 63) c = 63;
                clearance_[idx(x, y)] = uint8_t(c);
            }
        clearance_rev_ = revision_;
    }
    int blocked_count_ = 0;
    uint32_t revision_ = 0;
    std::vector<uint8_t> levels_;
    std::vector<uint8_t> ramps_;
    std::vector<uint8_t> cliff_;
    std::vector<uint8_t> ff_;
    int level_count_ = 0;
    int cliff_count_ = 0;
    int ff_count_ = 0;
};

inline constexpr int32_t kNavJumpCost = 40;

inline constexpr int32_t kNavFFCost = 30;

inline void find_path(const NavGrid& g, int sx, int sy, int gx, int gy,
                      std::vector<NavCell>& out, bool can_jump = false, bool crush_ff = false,
                      bool can_walk_cliffs = false, const Fixed* goal_point_x = nullptr,
                      const Fixed* goal_point_y = nullptr, Fixed radius = Fixed(), int leap_cells = 1,
                      bool nearest_fallback = false) {
    out.clear();
    if (!g.in_bounds(sx, sy) || !g.in_bounds(gx, gy)) return;
    int64_t goal_px = 0, goal_py = 0;
    {
        Fixed ccx, ccy;
        g.cell_center(gx, gy, ccx, ccy);
        goal_px = goal_point_x ? goal_point_x->raw() : ccx.raw();
        goal_py = goal_point_y ? goal_point_y->raw() : ccy.raw();
    }

    constexpr int kMaxRing = 8;
    auto nearest_free = [&g](int& cx, int& cy) -> bool {
        for (int r = 1; r <= kMaxRing; ++r)
            for (int dy = -r; dy <= r; ++dy)
                for (int dx = -r; dx <= r; ++dx) {
                    if (dx > -r && dx < r && dy > -r && dy < r) continue;
                    const int nx = cx + dx, ny = cy + dy;
                    if (!g.in_bounds(nx, ny) || g.is_blocked_cell(nx, ny)) continue;
                    cx = nx;
                    cy = ny;
                    return true;
                }
        return false;
    };

    if (g.is_blocked_cell(sx, sy)) {
        int fx = sx, fy = sy;
        if (!nearest_free(fx, fy)) return;
        sx = fx;
        sy = fy;
    }

    if (g.is_blocked_cell(gx, gy)) {
        struct Cand { int x, y; int64_t dg, ds; };
        std::vector<Cand> cands;
        const int64_t gpx = goal_px, gpy = goal_py;
        const int64_t half = g.cell_size().raw() / 2;
        for (int dy = -kMaxRing; dy <= kMaxRing; ++dy)
            for (int dx = -kMaxRing; dx <= kMaxRing; ++dx) {
                const int nx = gx + dx, ny = gy + dy;
                if (!g.in_bounds(nx, ny) || g.is_blocked_cell(nx, ny)) continue;
                if (!g.admits(nx, ny, radius)) continue;
                Fixed ccx, ccy;
                g.cell_center(nx, ny, ccx, ccy);
                const int64_t x0 = int64_t(ccx.raw()) - half, x1 = int64_t(ccx.raw()) + half;
                const int64_t y0 = int64_t(ccy.raw()) - half, y1 = int64_t(ccy.raw()) + half;
                const int64_t qx = gpx < x0 ? x0 - gpx : (gpx > x1 ? gpx - x1 : 0);
                const int64_t qy = gpy < y0 ? y0 - gpy : (gpy > y1 ? gpy - y1 : 0);
                const int64_t ddx = nx - sx, ddy = ny - sy;
                cands.push_back(Cand{nx, ny, qx * qx + qy * qy, ddx * ddx + ddy * ddy});
            }
        if (cands.empty()) return;
        std::stable_sort(cands.begin(), cands.end(), [](const Cand& a, const Cand& b) {
            return a.dg != b.dg ? a.dg < b.dg : a.ds < b.ds;
        });
        constexpr std::size_t kMaxCandidates = 8;
        for (std::size_t c = 0; c < cands.size() && c < kMaxCandidates; ++c) {
            find_path(g, sx, sy, cands[c].x, cands[c].y, out, can_jump, crush_ff, can_walk_cliffs, nullptr, nullptr, radius,
                      leap_cells, false);
            if (!out.empty()) return;
        }
        if (nearest_fallback)
            find_path(g, sx, sy, cands[0].x, cands[0].y, out, can_jump, crush_ff, can_walk_cliffs, nullptr, nullptr,
                      radius, leap_cells, true);
        return;
    }

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
    int best_idx = -1;
    int32_t best_h = kInf, best_g = kInf;
    while (!open.empty()) {
        const QN cur = open.top();
        open.pop();
        if (closed[size_t(cur.idx)]) continue;
        closed[size_t(cur.idx)] = 1;
        if (cur.h < best_h || (cur.h == best_h && gscore[size_t(cur.idx)] < best_g)) {
            best_h = cur.h;
            best_g = gscore[size_t(cur.idx)];
            best_idx = cur.idx;
        }
        if (cur.idx == goal) {
            found = true;
            break;
        }
        const int cx = cur.idx % W, cy = cur.idx / W;
        for (int k = 0; k < 8; ++k) {
            const int nx = cx + DX[k], ny = cy + DY[k];
            if (!g.in_bounds(nx, ny)) continue;
            bool via_ff = false;
            if (g.is_blocked_cell(nx, ny)) {
                if (crush_ff && g.has_ff() && g.is_ff_cell(nx, ny)) via_ff = true;
                else continue;
            }
            if (!via_ff && goal != index(nx, ny) && !g.admits(nx, ny, radius)) continue;
            const bool diag = (DX[k] != 0 && DY[k] != 0);
            if (diag) {
                if (g.is_blocked_cell(cx + DX[k], cy) ||
                    g.is_blocked_cell(cx, cy + DY[k]))
                    continue;
            }
            const int ni = index(nx, ny);
            if (closed[size_t(ni)]) continue;
            int32_t step_cost = (diag ? 14 : 10) + (via_ff ? kNavFFCost : 0);
            if (!g.can_traverse(cx, cy, nx, ny, can_jump, can_walk_cliffs)) continue;
            if (g.is_leap_edge(cx, cy, nx, ny, can_walk_cliffs))
                step_cost += kNavJumpCost;
            const int32_t tentative = gscore[size_t(cur.idx)] + step_cost;
            if (tentative < gscore[size_t(ni)]) {
                gscore[size_t(ni)] = tentative;
                came[size_t(ni)] = cur.idx;
                const int32_t hh = heur(nx, ny);
                open.push(QN{tentative + hh, hh, ni});
            }
        }
        if (can_jump && g.has_cliffs() && leap_cells > 1) {
            for (int k = 0; k < 8; ++k) {
                int run = 0, lx = cx, ly = cy;
                for (;;) {
                    lx += DX[k];
                    ly += DY[k];
                    if (!g.in_bounds(lx, ly) || !g.is_cliff_cell(lx, ly)) break;
                    if (++run > leap_cells) break;
                }
                if (run < 1 || run > leap_cells) continue;
                if (!g.in_bounds(lx, ly) || g.is_blocked_cell(lx, ly)) continue;
                if (!g.admits(lx, ly, radius) && index(lx, ly) != goal) continue;
                const int li = index(lx, ly);
                if (closed[size_t(li)]) continue;
                const bool diag = DX[k] != 0 && DY[k] != 0;
                const int32_t tentative = gscore[size_t(cur.idx)] + (diag ? 14 : 10) * (run + 1) + kNavJumpCost;
                if (tentative < gscore[size_t(li)]) {
                    gscore[size_t(li)] = tentative;
                    came[size_t(li)] = cur.idx;
                    const int32_t hh = heur(lx, ly);
                    open.push(QN{tentative + hh, hh, li});
                }
            }
        } else if (can_jump && g.has_cliffs()) {
            static const int JX[4] = {0, -1, 1, 0};
            static const int JY[4] = {-1, 0, 0, 1};
            for (int k = 0; k < 4; ++k) {
                const int wx = cx + JX[k], wy = cy + JY[k];
                const int lx = cx + 2 * JX[k], ly = cy + 2 * JY[k];
                if (!g.is_cliff_cell(wx, wy)) continue;
                if (!g.in_bounds(lx, ly) || g.is_blocked_cell(lx, ly)) continue;
                const int li = index(lx, ly);
                if (closed[size_t(li)]) continue;
                const int32_t tentative = gscore[size_t(cur.idx)] + 20 + kNavJumpCost;
                if (tentative < gscore[size_t(li)]) {
                    gscore[size_t(li)] = tentative;
                    came[size_t(li)] = cur.idx;
                    const int32_t hh = heur(lx, ly);
                    open.push(QN{tentative + hh, hh, li});
                }
            }
        }
    }

    int end = goal;
    if (!found) {
        if (!nearest_fallback || best_idx < 0) return;
        end = best_idx;
    }
    std::vector<NavCell> rev;
    for (int at = end; at != -1; at = came[size_t(at)])
        rev.push_back(NavCell{at % W, at / W});
    for (size_t i = rev.size(); i-- > 0;) out.push_back(rev[i]);
}

}
