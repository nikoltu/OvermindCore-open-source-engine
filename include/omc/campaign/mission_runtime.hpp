#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <utility>

#include "omc/campaign/trigger_vm.hpp"
#include "omc/formats/map_archive.hpp"
#include "omc/sim/world.hpp"

namespace omc::data {
class Catalog;
}

namespace omc::campaign {

struct MissionRunReport {
    std::size_t units_before = 0;
    std::size_t units_after = 0;
    std::size_t created = 0;
    std::size_t killed = 0;
    std::size_t created_defaulted_loc = 0;
    std::size_t alliance_changes = 0;
    std::size_t resource_changes = 0;
    std::map<std::string, std::size_t> wired;
    std::map<int, std::pair<int, int>> banks_before;
    std::map<int, std::pair<int, int>> banks_after;
};

class MissionRuntime {
  public:
    MissionRuntime(const data::Catalog& cat, const formats::LoadedMap& map);

    sim::World& world() { return world_; }
    const sim::World& world() const { return world_; }

    MissionRunReport run_init(const TriggerModel& model, VmStats* vm_stats = nullptr);

  private:
    const data::Catalog& cat_;
    sim::World world_;
};

}
