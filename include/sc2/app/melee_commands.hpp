#pragma once

#include <cstdint>

#include "sc2/sim/fixed.hpp"
#include "sc2/sim/world.hpp"

namespace sc2::app {

struct ActionCheck {
    bool producer_ok = false;
    bool minerals_ok = false;
    bool gas_ok = false;
    bool supply_ok = false;
    bool pending_ok = false;

    bool allowed() const {
        return producer_ok && minerals_ok && gas_ok && supply_ok && pending_ok;
    }
};

struct MeleeCommands {
    static constexpr int kPendingCap = 8;

    static ActionCheck can_train(const sim::World& w, uint32_t producer,
                                 const sim::Unit& proto, int mineral_cost,
                                 int gas_cost, int pending_cap = kPendingCap);

    static bool train(sim::World& w, uint32_t producer, const sim::Unit& proto,
                      int mineral_cost, int gas_cost, sim::Fixed build_time,
                      int pending_cap = kPendingCap, int count = 1);

    static ActionCheck can_build(const sim::World& w, int player,
                                 const sim::Unit& proto, int mineral_cost,
                                 int gas_cost);

    static bool build(sim::World& w, int player, const sim::Unit& proto, sim::Fixed x,
                      sim::Fixed y, int mineral_cost, int gas_cost);
};

}
