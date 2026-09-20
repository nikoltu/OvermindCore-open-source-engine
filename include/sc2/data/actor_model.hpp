#pragma once

#include <optional>
#include <string>

#include "sc2/data/catalog.hpp"

namespace sc2::data {

std::optional<std::string> resolve_unit_model(const Catalog& catalog,
                                              const std::string& unit_id);

std::optional<std::string> resolve_build_model(const Catalog& catalog,
                                               const std::string& unit_name);

}
