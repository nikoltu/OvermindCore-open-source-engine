#pragma once

#include <cstdint>
#include <vector>

#include "omc/sim/fixed.hpp"
#include "omc/sim/nav.hpp"

namespace omc::sim {

class ForceFieldSystem {
public:
    int place(NavGrid& grid, Fixed x, Fixed y, Fixed radius, Fixed duration) {
        if (!grid.configured() || radius.raw() <= 0) return -1;
        Field f;
        f.timer = duration.raw() > 0 ? duration : Fixed::from_raw(1);

        int ccx, ccy;
        grid.to_cell(x, y, ccx, ccy);
        int cr = radius.floor() + 1;
        const Fixed r2 = radius * radius;
        for (int cy = ccy - cr; cy <= ccy + cr; ++cy)
            for (int cx = ccx - cr; cx <= ccx + cr; ++cx) {
                if (!grid.in_bounds(cx, cy)) continue;
                Fixed wx, wy;
                grid.cell_center(cx, cy, wx, wy);
                const Fixed dx = wx - x, dy = wy - y;
                if (dx * dx + dy * dy > r2) continue;
                if (grid.is_blocked_cell(cx, cy)) continue;
                grid.block_cell(cx, cy);
                f.cells.push_back(Cell{cx, cy});
            }
        fields_.push_back(std::move(f));
        return int(fields_.size()) - 1;
    }

    int active_count() const {
        int n = 0;
        for (const Field& f : fields_)
            if (f.active) ++n;
        return n;
    }

    void advance(NavGrid& grid, Fixed dt) {
        for (Field& f : fields_) {
            if (!f.active) continue;
            f.timer = f.timer - dt;
            if (f.timer.raw() > 0) continue;
            f.active = false;
            for (const Cell& c : f.cells) grid.set_blocked(c.x, c.y, false);
            f.cells.clear();
        }
        if (active_count() == 0) fields_.clear();
    }

private:
    struct Cell {
        int x, y;
    };
    struct Field {
        Fixed timer;
        std::vector<Cell> cells;
        bool active = true;
    };
    std::vector<Field> fields_;
};

}
