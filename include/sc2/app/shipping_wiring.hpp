#pragma once

#include <string>
#include <vector>

#include "sc2/app/melee.hpp"
#include "sc2/app/sandbox_game.hpp"

namespace sc2::data {
class Catalog;
class UpgradeCatalog;
class Production;
}

namespace sc2::app {

class MeleeGame;

void apply_shipping_wiring(MeleeGame& game, const data::Catalog& cat,
                           const data::UpgradeCatalog* up = nullptr,
                           const MeleeSetup* setup = nullptr,
                           const std::vector<Race>* races = nullptr);

std::vector<std::string> techlab_gated_units(const data::Catalog& cat, const data::Production& prod);

}
