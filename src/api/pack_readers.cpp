#include "omc/api/truth_pack.hpp"

namespace omc::api {

namespace {

UnitTypePackReader g_unit_types = nullptr;
AbilityPackReader g_abilities = nullptr;

const char kNoReader[] = "this build has no id pack reader; the API names units and abilities only when one is linked in";

}

void set_pack_readers(UnitTypePackReader unit_types, AbilityPackReader abilities) {
    g_unit_types = unit_types;
    g_abilities = abilities;
}

UnitTypePack load_unit_type_pack(const std::string& root, std::uint32_t data_build) {
    if (g_unit_types) return g_unit_types(root, data_build);
    UnitTypePack pack;
    pack.data_build = data_build;
    pack.error = kNoReader;
    return pack;
}

AbilityPack load_ability_pack(const std::string& root, std::uint32_t data_build) {
    if (g_abilities) return g_abilities(root, data_build);
    AbilityPack pack;
    pack.data_build = data_build;
    pack.error = kNoReader;
    return pack;
}

}
