#pragma once

#include <string>
#include <vector>

#include "omc/app/melee.hpp"
#include "omc/app/melee_game.hpp"
#include "omc/app/race.hpp"
#include "omc/render/input.hpp"
#include "omc/render/scene.hpp"
#include "omc/sim/world.hpp"

namespace omc::data {
class Catalog;
class UpgradeCatalog;
class Production;
}

namespace omc::app {

class MeleeGame;

void apply_shipping_wiring(MeleeGame& game, const data::Catalog& cat,
                           const data::UpgradeCatalog* up = nullptr,
                           const MeleeSetup* setup = nullptr,
                           const std::vector<Race>* races = nullptr);

std::vector<std::string> techlab_gated_units(const data::Catalog& cat, const data::Production& prod);

}
