#include "sc2/ai/macro.hpp"

#include "sc2/app/spawn.hpp"
#include "sc2/data/economy.hpp"

namespace sc2::ai {

MacroController::MacroController(const data::Catalog& cat, const data::Production& prod,
                                int player, MacroConfig cfg)
    : cat_(cat), prod_(prod), player_(player), cfg_(std::move(cfg)) {
    army_proto_ = app::make_unit(cat_, cfg_.army_unit, player_);
    army_cost_ = data::unit_cost(cat_, cfg_.army_unit);
    supply_proto_ = app::make_unit(cat_, cfg_.supply_unit, player_);
    supply_proto_.speed = sim::Fixed();
    supply_cost_ = data::unit_cost(cat_, cfg_.supply_unit);
}

bool MacroController::is_worker(uint32_t id) const {
    for (uint32_t w : base_.workers)
        if (w == id) return true;
    return false;
}

void MacroController::update(sim::World& w) {
    using sim::Fixed;

    for (uint32_t wid : base_.workers) {
        if (wid >= w.units().size()) continue;
        sim::Unit& u = w.unit(wid);
        if (u.alive && u.orders.empty()) {
            u.home_depot = base_.command_center;
            u.gather_target = base_.mineral;
            w.issue(wid,
                    sim::Order{sim::OrderType::Gather, Fixed(), Fixed(), sim::kNoTarget});
        }
    }

    Fixed used = w.supply_used(player_), cap = w.supply_max(player_);
    bool near_cap = (used + Fixed::from_int(2)) >= cap;
    if (near_cap && base_.command_center < w.units().size() &&
        w.unit(base_.command_center).alive &&
        w.player(player_).minerals >= supply_cost_.minerals &&
        int(w.pending_count()) < cfg_.max_pending) {
        w.queue_train(base_.command_center, supply_proto_, supply_cost_.minerals,
                      supply_cost_.gas, cfg_.build_time);
        return;
    }

    if (base_.producer < w.units().size() && w.unit(base_.producer).alive) {
        data::TechState tech;
        bool tech_ok =
            prod_.can_produce(cat_, cfg_.producer_ability, cfg_.army_unit, tech);
        bool afford = w.player(player_).minerals >= army_cost_.minerals &&
                      w.player(player_).gas >= army_cost_.gas;
        bool supply = (w.supply_used(player_) + army_cost_.supply) <=
                      w.supply_max(player_);
        if (tech_ok && afford && supply &&
            int(w.pending_count()) < cfg_.max_pending) {
            w.queue_train(base_.producer, army_proto_, army_cost_.minerals,
                          army_cost_.gas, cfg_.build_time);
        }
    }

    for (const sim::Unit& u : w.units()) {
        if (!u.alive || u.player != player_) continue;
        if (u.wpn_damage.raw() <= 0 || u.speed.raw() <= 0) continue;
        if (is_worker(u.id) || !u.orders.empty()) continue;
        w.issue(u.id, sim::Order{sim::OrderType::AttackMove, base_.attack_x,
                                 base_.attack_y, sim::kNoTarget});
    }
}

}
