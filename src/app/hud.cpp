#include "sc2/app/hud.hpp"

#include "sc2/sim/world.hpp"

namespace sc2::app {

HudContent HudModel::build(const sim::World& world, int player,
                           const std::vector<std::uint32_t>& selection) {
    HudContent c;

    const sim::Player& pl = world.player(player);
    c.minerals = pl.minerals;
    c.gas = pl.gas;
    c.supply_used = world.supply_used(player).round();
    c.supply_max = world.supply_max(player).round();

    c.selection_count = selection.size();
    const std::vector<sim::Unit>& units = world.units();
    std::size_t owned = 0;
    for (std::uint32_t id : selection) {
        if (id >= units.size()) continue;
        const sim::Unit& u = units[id];
        if (u.alive && u.player == player) ++owned;
    }
    c.selection_owned = owned;
    c.selection_summary =
        owned == 0 ? std::string()
                   : std::to_string(owned) + (owned == 1 ? " unit" : " units");

    auto clamp01 = [](double v) { return v < 0 ? 0.0f : (v > 1 ? 1.0f : float(v)); };
    for (std::uint32_t id : selection) {
        if (id >= units.size()) continue;
        const sim::Unit& u = units[id];
        if (!u.alive || u.player != player) continue;
        c.lead_unit = id;
        WireframeState& wf = c.lead_wireframe;
        wf.life_frac =
            u.hp_max.raw() > 0 ? clamp01(u.hp.to_double() / u.hp_max.to_double()) : 0.0f;
        wf.energy_frac =
            u.energy_max.raw() > 0 ? clamp01(u.energy.to_double() / u.energy_max.to_double())
                                   : 0.0f;
        wf.constructing = u.constructing;
        wf.build_progress = float(u.build_progress.to_double());
        break;
    }
    return c;
}

}
