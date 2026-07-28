#pragma once

#include <optional>
#include <string>

#include "sc2/data/catalog.hpp"
#include "sc2/sim/world.hpp"

namespace sc2::data {

std::optional<sim::VeterancyDef> load_veterancy(const Catalog& cat,
                                                const std::string& id);

}
