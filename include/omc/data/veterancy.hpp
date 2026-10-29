#pragma once

#include <optional>
#include <string>

#include "omc/data/catalog.hpp"
#include "omc/sim/world.hpp"

namespace omc::data {

std::optional<sim::VeterancyDef> load_veterancy(const Catalog& cat,
                                                const std::string& id);

}
