#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace omc::api {

struct UnitTypePack {
    bool loaded = false;
    std::uint32_t data_build = 0;
    std::string path;
    std::string error;
    std::unordered_map<std::string, std::uint32_t> id_by_name;
};

UnitTypePack load_unit_type_pack(const std::string& root, std::uint32_t data_build);

struct AbilityPack {
    bool loaded = false;
    std::uint32_t data_build = 0;
    std::string path;
    std::string error;
    std::unordered_map<std::string, std::uint32_t> id_by_name;
    std::unordered_map<std::uint32_t, std::uint32_t> remaps_to;
};

AbilityPack load_ability_pack(const std::string& root, std::uint32_t data_build);

using UnitTypePackReader = UnitTypePack (*)(const std::string& root, std::uint32_t data_build);
using AbilityPackReader = AbilityPack (*)(const std::string& root, std::uint32_t data_build);
void set_pack_readers(UnitTypePackReader unit_types, AbilityPackReader abilities);

}
