#include "sc2/app/melee.hpp"

#include <utility>

namespace sc2::app {

using sim::Fixed;
using sim::Order;
using sim::OrderType;
using sim::Unit;

namespace {

Fixed step_toward(Fixed from, Fixed to, int amount) {
    if (to > from) return Fixed::from_int(amount);
    if (to < from) return Fixed::from_int(-amount);
    return Fixed();
}

}

MeleeLayout build_melee(sim::World& w, const MeleeSetup& s) {
    MeleeLayout layout;
    const size_t nplayers = s.players.size();

    auto add = [&](Unit u, const char* type) -> uint32_t {
        uint32_t id = w.add_unit(std::move(u));
        if (layout.unit_type.size() <= id) layout.unit_type.resize(id + 1);
        layout.unit_type[id] = type;
        return id;
    };

    for (size_t p = 0; p < nplayers; ++p) {
        const MeleePlayerSetup& ps = s.players[p];
        const MeleePlayerSetup& enemy = s.players[(p + 1) % nplayers];
        MeleePlayerLayout out;

        Unit hall = ps.town_hall;
        hall.player = int32_t(p);
        hall.x = ps.start.x;
        hall.y = ps.start.y;
        hall.speed = Fixed();
        out.town_hall = add(std::move(hall), "CommandCenter");

        for (const MeleePos& mp : ps.mineral_positions) {
            Unit field = ps.mineral_field;
            field.player = -1;
            field.x = mp.x;
            field.y = mp.y;
            field.speed = Fixed();
            out.minerals.push_back(add(std::move(field), "MineralField"));
        }

        for (const MeleePos& gp : ps.geyser_positions) {
            Unit gy = ps.geyser_field;
            gy.player = -1;
            gy.x = gp.x;
            gy.y = gp.y;
            gy.speed = Fixed();
            out.geysers.push_back(add(std::move(gy), "VespeneGeyser"));
        }

        const uint32_t gather =
            out.minerals.empty() ? sim::kNoTarget : out.minerals.front();
        const Fixed hall_r = ps.town_hall.radius;
        for (int i = 0; i < ps.worker_count; ++i) {
            Unit scv = ps.worker;
            scv.player = int32_t(p);
            const uint32_t node = out.minerals.empty()
                ? sim::kNoTarget
                : out.minerals[std::size_t(i) % out.minerals.size()];
            Fixed dx, dy;
            if (!ps.mineral_positions.empty()) {
                const MeleePos& mp = ps.mineral_positions[std::size_t(i) % ps.mineral_positions.size()];
                dx = mp.x - ps.start.x;
                dy = mp.y - ps.start.y;
            }
            const Fixed d = sim::isqrt(dx * dx + dy * dy);
            const Fixed off = hall_r + Fixed::from_ratio(3, 4);
            const Fixed sx = Fixed::from_ratio(i % 3, 2) - Fixed::from_ratio(1, 2);
            const Fixed sy = Fixed::from_ratio((i / 3) % 3, 2) - Fixed::from_ratio(1, 2);
            if (d.raw() > 0) {
                scv.x = ps.start.x + dx * off / d + sx;
                scv.y = ps.start.y + dy * off / d + sy;
            } else {
                scv.x = ps.start.x + sx;
                scv.y = ps.start.y + off + sy;
            }
            uint32_t id = add(std::move(scv), "SCV");
            w.unit(id).home_depot = out.town_hall;
            w.unit(id).gather_target = node;
            w.issue(id, Order{OrderType::Gather, Fixed(), Fixed(), sim::kNoTarget});
            out.workers.push_back(id);
        }

        if (ps.gas_worker_count > 0 && !out.geysers.empty() && !ps.geyser_positions.empty()) {
            const uint32_t geyser = out.geysers.front();
            const MeleePos& gpos = ps.geyser_positions.front();
            Fixed dx = gpos.x - ps.start.x, dy = gpos.y - ps.start.y;
            const Fixed d = sim::isqrt(dx * dx + dy * dy);
            const Fixed off = hall_r + Fixed::from_ratio(3, 4);
            for (int i = 0; i < ps.gas_worker_count; ++i) {
                Unit scv = ps.worker;
                scv.player = int32_t(p);
                scv.gathers_gas = true;
                const Fixed sx = Fixed::from_ratio(i % 3, 2) - Fixed::from_ratio(1, 2);
                if (d.raw() > 0) {
                    scv.x = ps.start.x + dx * off / d + sx;
                    scv.y = ps.start.y + dy * off / d;
                } else {
                    scv.x = ps.start.x + sx;
                    scv.y = ps.start.y - off;
                }
                uint32_t id = add(std::move(scv), "SCV");
                w.unit(id).home_depot = out.town_hall;
                w.unit(id).gather_target = geyser;
                w.issue(id, Order{OrderType::Gather, Fixed(), Fixed(), sim::kNoTarget});
                out.workers.push_back(id);
            }
        }

        out.barracks = sim::kNoTarget;
        Unit prod_proto = ps.producer;
        prod_proto.speed = Fixed();
        out.config.producer_unit =
            ai::BuildItem{prod_proto, ps.producer_mineral_cost, ps.producer_gas_cost,
                          ps.producer_build_time, false};
        out.config.has_producer = true;

        w.player(int32_t(p)).minerals = ps.start_minerals;

        out.config.army_unit =
            ai::BuildItem{ps.army_unit, ps.army_mineral_cost, ps.army_gas_cost,
                          ps.army_build_time, true};
        out.config.army_threshold = ps.army_threshold;
        {
            Unit wproto = ps.worker;
            wproto.is_worker = true;
            out.config.worker_unit = ai::BuildItem{wproto, ps.worker_mineral_cost, 0,
                                                   ps.worker_build_time, false};
            out.config.worker_target =
                int(ps.mineral_positions.size()) * 2 + ps.gas_worker_count;
        }
        out.config.rally_x = ps.start.x + step_toward(ps.start.x, enemy.start.x, 10);
        out.config.rally_y = ps.start.y + step_toward(ps.start.y, enemy.start.y, 10);
        out.config.enemy_x = enemy.start.x;
        out.config.enemy_y = enemy.start.y;

        out.base.producer = out.barracks;
        out.base.depot = out.town_hall;
        out.base.mineral = gather;
        out.base.workers = out.workers;

        layout.players.push_back(std::move(out));
    }

    if (layout.unit_type.size() < w.units().size())
        layout.unit_type.resize(w.units().size());
    return layout;
}

}
