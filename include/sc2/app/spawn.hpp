#pragma once

#include <string>

#include "sc2/data/catalog.hpp"
#include "sc2/sim/world.hpp"

namespace sc2::app {

sim::Unit make_unit(const data::Catalog& cat, const std::string& unit_id,
                    int player, sim::Fixed x = sim::Fixed(),
                    sim::Fixed y = sim::Fixed());

}
