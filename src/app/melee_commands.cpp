#include "omc/app/melee_commands.hpp"

namespace omc::app {

using sim::Fixed;

ActionCheck MeleeCommands::can_train(const sim::World& w, uint32_t producer,
                                     const sim::Unit& proto, int mineral_cost,
                                     int gas_cost, int pending_cap) {
    ActionCheck c;
    if (producer >= w.units().size() || !w.units()[producer].alive) return c;
    c.producer_ok = true;

    const int32_t pl = w.units()[producer].player;
    const sim::Player& p = w.player(pl);
    c.minerals_ok = p.minerals >= mineral_cost;
    c.gas_ok = p.gas >= gas_cost;
    c.supply_ok = w.supply_room(pl, proto.supply_uses);
    const int depth = w.queue_depth(producer) > 0 && w.units()[producer].queue_size > 0 ? w.queue_depth(producer)
                                                                                         : pending_cap;
    c.pending_ok = int(w.queue_count(producer)) < depth;
    return c;
}

bool MeleeCommands::train(sim::World& w, uint32_t producer, const sim::Unit& proto,
                          int mineral_cost, int gas_cost, Fixed build_time,
                          int pending_cap, int count) {
    const int n = count > 1 ? count : 1;
    if (!can_train(w, producer, proto, mineral_cost * n, gas_cost * n, pending_cap).allowed())
        return false;
    return w.queue_train(producer, proto, mineral_cost, gas_cost, build_time, n);
}

ActionCheck MeleeCommands::can_build(const sim::World& w, int player,
                                     const sim::Unit& proto, int mineral_cost,
                                     int gas_cost) {
    ActionCheck c;
    c.producer_ok = true;
    c.pending_ok = true;

    const sim::Player& p = w.player(player);
    c.minerals_ok = p.minerals >= mineral_cost;
    c.gas_ok = p.gas >= gas_cost;
    c.supply_ok = w.supply_room(player, proto.supply_uses);
    return c;
}

bool MeleeCommands::build(sim::World& w, int player, const sim::Unit& proto, Fixed x,
                          Fixed y, int mineral_cost, int gas_cost) {
    if (!can_build(w, player, proto, mineral_cost, gas_cost).allowed()) return false;

    sim::Player& p = w.player(player);
    p.minerals -= mineral_cost;
    p.gas -= gas_cost;

    sim::Unit u = proto;
    u.player = player;
    u.x = x;
    u.y = y;
    u.speed = Fixed();
    w.add_unit(u);
    return true;
}

}
