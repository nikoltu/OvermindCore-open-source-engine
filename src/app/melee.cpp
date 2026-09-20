#include "sc2/app/melee.hpp"

#include <algorithm>

#include <utility>

#include "sc2/app/spawn.hpp"

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
    auto face = [&](uint32_t id, float yaw) {
        if (layout.unit_facing.size() <= id) {
            layout.unit_facing.resize(id + 1, 0.0f);
            layout.unit_facing_set.resize(id + 1, 0);
        }
        layout.unit_facing[id] = yaw;
        layout.unit_facing_set[id] = 1;
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
        face(out.town_hall, ps.town_hall_facing);

        for (std::size_t i = 0; i < ps.mineral_positions.size(); ++i) {
            const MeleePos& mp = ps.mineral_positions[i];
            Unit field = i < ps.mineral_protos.size() ? ps.mineral_protos[i] : ps.mineral_field;
            field.player = -1;
            field.x = mp.x;
            field.y = mp.y;
            field.speed = Fixed();
            const uint32_t id = add(std::move(field), "MineralField");
            face(id, kUnauthoredFacing);
            out.minerals.push_back(id);
        }

        for (std::size_t i = 0; i < ps.geyser_positions.size(); ++i) {
            const MeleePos& gp = ps.geyser_positions[i];
            Unit gy = i < ps.geyser_protos.size() ? ps.geyser_protos[i] : ps.geyser_field;
            gy.player = -1;
            gy.x = gp.x;
            gy.y = gp.y;
            gy.speed = Fixed();
            const uint32_t id = add(std::move(gy), "VespeneGeyser");
            face(id, kUnauthoredFacing);
            out.geysers.push_back(id);
        }

        const uint32_t gather =
            out.minerals.empty() ? sim::kNoTarget : out.minerals.front();
        const Fixed hall_r = ps.town_hall.radius;
        std::int64_t mcx = 0, mcy = 0;
        for (const MeleePos& mp : ps.mineral_positions) {
            mcx += mp.x.raw();
            mcy += mp.y.raw();
        }
        const std::int64_t n_min = std::int64_t(ps.mineral_positions.size());
        if (n_min > 0) {
            mcx /= n_min;
            mcy /= n_min;
        } else {
            mcx = ps.start.x.raw();
            mcy = ps.start.y.raw();
        }
        auto dist2_to_minerals = [&](const std::pair<int, int>& off) {
            const std::int64_t dx = std::int64_t(ps.start.x.raw()) + std::int64_t(off.first) * Fixed::kOne - mcx;
            const std::int64_t dy = std::int64_t(ps.start.y.raw()) + std::int64_t(off.second) * Fixed::kOne - mcy;
            return (dx >> 6) * (dx >> 6) + (dy >> 6) * (dy >> 6);
        };
        const std::size_t ring = ps.worker_offsets.size();
        const bool authored_ring = ring > 0 && std::size_t(ps.worker_count) <= ring;
        std::size_t window = 0;
        if (authored_ring) {
            std::int64_t best = -1;
            for (std::size_t k = 0; k < ring; ++k) {
                std::int64_t sum = 0;
                for (int i = 0; i < ps.worker_count; ++i) sum += dist2_to_minerals(ps.worker_offsets[(k + std::size_t(i)) % ring]);
                if (best < 0 || sum < best) {
                    best = sum;
                    window = k;
                }
            }
        }
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
            if (authored_ring) {
                const std::pair<int, int>& o = ps.worker_offsets[(window + std::size_t(i)) % ring];
                scv.x = ps.start.x + Fixed::from_int(o.first);
                scv.y = ps.start.y + Fixed::from_int(o.second);
            } else if (d.raw() > 0) {
                scv.x = ps.start.x + dx * off / d + sx;
                scv.y = ps.start.y + dy * off / d + sy;
            } else {
                scv.x = ps.start.x + sx;
                scv.y = ps.start.y + off + sy;
            }
            uint32_t id = add(std::move(scv), "SCV");
            w.unit(id).home_depot = out.town_hall;
            w.unit(id).gather_target = node;
            if (node != sim::kNoTarget)
                w.issue(id, sim::Order{sim::OrderType::Gather, Fixed(), Fixed(), sim::kNoTarget});
            out.workers.push_back(id);
        }

        for (const MeleePlayerSetup::StartExtra& ex : ps.start_extras) {
            std::vector<std::size_t> order;
            for (std::size_t k = 0; k < ex.offsets.size(); ++k) order.push_back(k);
            std::stable_sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) {
                return dist2_to_minerals(ex.offsets[a]) > dist2_to_minerals(ex.offsets[b]);
            });
            for (int c = 0; c < ex.count; ++c) {
                Unit extra = ex.proto;
                extra.player = int32_t(p);
                extra.alive = true;
                if (!order.empty()) {
                    const std::pair<int, int>& o = ex.offsets[order[std::size_t(c) % order.size()]];
                    extra.x = ps.start.x + Fixed::from_int(o.first);
                    extra.y = ps.start.y + Fixed::from_int(o.second);
                } else {
                    extra.x = ps.start.x;
                    extra.y = ps.start.y;
                }
                add(std::move(extra), ex.type.c_str());
            }
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
                if (geyser != sim::kNoTarget)
                    w.issue(id, sim::Order{sim::OrderType::Gather, Fixed(), Fixed(), sim::kNoTarget});
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
        if (!s.expansion_sites.empty()) {
            Unit hall_proto = ps.town_hall;
            hall_proto.speed = Fixed();
            out.config.has_expansion = true;
            out.config.expansion_unit = ai::BuildItem{hall_proto, ps.town_hall_mineral_cost, 0, ps.town_hall_build_time, false};
            for (const MeleePos& e : s.expansion_sites) out.config.expansion_sites.push_back({e.x, e.y});
        }

        if (ps.has_tech) {
            Unit tech_proto = ps.tech_unit;
            tech_proto.speed = Fixed();
            out.config.build_order.push_back(
                ai::BuildItem{tech_proto, ps.tech_mineral_cost, ps.tech_gas_cost,
                              ps.tech_build_time, false});
        }

        w.player(int32_t(p)).minerals = ps.start_minerals;

        out.config.army_unit =
            ai::BuildItem{ps.army_unit, ps.army_mineral_cost, ps.army_gas_cost,
                          ps.army_build_time, true, ps.army_count};
        if (ps.has_army_unit2) {
            out.config.army_unit2 =
                ai::BuildItem{ps.army_unit2, ps.army2_mineral_cost, ps.army2_gas_cost,
                              ps.army2_build_time, true};
            out.config.has_army_unit2 = true;
            out.config.army_unit2_gate = ps.army_unit2_gate;
            out.config.tech_chain = ps.tech_chain;
        }
        out.config.army_threshold = ps.army_threshold;
        for (const auto& ph : ps.wave_phases)
            out.config.wave_phases.push_back(ai::MeleeConfig::WavePhase{ph.first, ph.second});
        out.config.stock = ps.stock;
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

    for (std::size_t i = 0; i < s.neutrals.size(); ++i) {
        const uint32_t id = add(s.neutrals[i], s.neutrals[i].catalog_id.c_str());
        if (i < s.neutral_facing.size()) face(id, s.neutral_facing[i]);
    }

    if (layout.unit_type.size() < w.units().size())
        layout.unit_type.resize(w.units().size());
    return layout;
}

}
