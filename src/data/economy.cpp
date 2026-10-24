#include "sc2/data/economy.hpp"

#include <algorithm>
#include <cstdlib>
#include <vector>

namespace sc2::data {

using sim::Fixed;

static int cost_resource(const Catalog& cat, const std::string& unit_id,
                         const std::string& which) {
    int value = 0;
    for (const core::XmlNode& c : cat.resolved("CUnit", unit_id)) {
        if (c.name != "CostResource") continue;
        if (c.attr_or("index", "") == which)
            value = int(Fixed::parse(c.attr_or("value", "0")).round());
    }
    return value;
}

static Fixed food(const Catalog& cat, const std::string& unit_id) {
    auto s = cat.scalar("CUnit", unit_id, "Food");
    return (s && !s->empty()) ? Fixed::parse(*s) : Fixed();
}

UnitCost unit_cost(const Catalog& cat, const std::string& unit_id) {
    UnitCost c;
    c.minerals = cost_resource(cat, unit_id, "Minerals");
    c.gas = cost_resource(cat, unit_id, "Vespene");
    Fixed f = food(cat, unit_id);
    if (f.raw() < 0) c.supply = -f;
    return c;
}

UnitCost build_charge(const Catalog& cat, const std::string& structure, std::string builder, std::string abil) {
    UnitCost c = unit_cost(cat, structure);
    static const bool kOld = std::getenv("SC2_ZERG_BUILD_FULL_COST") != nullptr;
    if (kOld) return c;
    if (abil.empty()) abil = cat.build_ability_of(structure);
    if (abil.empty() || !cat.ability_flag(abil, "PeonKillFinish")) return c;
    if (builder.empty()) {
        std::vector<std::string> ids = cat.ids("CUnit");
        std::sort(ids.begin(), ids.end());
        for (const std::string& id : ids) {
            const std::vector<std::string> ab = cat.array_links("CUnit", id, "AbilArray");
            if (std::find(ab.begin(), ab.end(), abil) != ab.end()) {
                builder = id;
                break;
            }
        }
    }
    if (builder.empty()) return c;
    const UnitCost b = unit_cost(cat, builder);
    c.minerals = std::max(0, c.minerals - b.minerals);
    c.gas = std::max(0, c.gas - b.gas);
    return c;
}

Fixed supply_provided(const Catalog& cat, const std::string& unit_id) {
    Fixed f = food(cat, unit_id);
    return f.raw() > 0 ? f : Fixed();
}

bool can_afford(const Catalog& cat, const PlayerEconomy& eco,
                const std::string& unit_id) {
    UnitCost c = unit_cost(cat, unit_id);
    if (eco.minerals < c.minerals || eco.gas < c.gas) return false;
    if ((eco.supply_used + c.supply) > eco.supply_max) return false;
    return true;
}

bool try_train(const Catalog& cat, const Production& prod, PlayerEconomy& eco,
               const TechState& tech, const std::string& ability,
               const std::string& product) {
    if (!prod.can_produce(cat, ability, product, tech)) return false;
    if (!can_afford(cat, eco, product)) return false;
    UnitCost c = unit_cost(cat, product);
    eco.minerals -= c.minerals;
    eco.gas -= c.gas;
    eco.supply_used = eco.supply_used + c.supply;
    return true;
}

}
