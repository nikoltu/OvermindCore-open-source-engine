#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "sc2/formats/map_archive.hpp"

namespace sc2::casc {
class Storage;
}

namespace sc2::data {
class Catalog;
}

namespace sc2::campaign {

std::vector<std::string> liberty_campaign_roots();

formats::LoadedMap load_mission_map(const casc::Storage& store, const std::string& map_prefix);

std::string mission_local_data_root(const std::string& map_prefix);

std::vector<std::uint8_t> load_mission_trigger_chunk(const casc::Storage& store,
                                                     const std::string& map_prefix);

}
