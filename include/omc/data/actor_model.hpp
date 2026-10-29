#pragma once

#include <array>
#include <optional>
#include <string>

#include "omc/data/catalog.hpp"

namespace omc::data {

std::string unit_actor_id(const Catalog& catalog, const std::string& unit_id);

std::optional<std::string> resolve_unit_model(const Catalog& catalog,
                                              const std::string& unit_id);

std::optional<std::string> resolve_build_model(const Catalog& catalog,
                                               const std::string& unit_name);

std::array<float, 3> resolve_unit_model_scale(const Catalog& catalog, const std::string& unit_id);

std::array<float, 3> resolve_model_scale(const Catalog& catalog, const std::string& model_id);
std::array<float, 3> resolve_doodad_model_scale(const Catalog& catalog, const std::string& doodad_type);

}
