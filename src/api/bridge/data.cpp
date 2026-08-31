#include <cstdio>
#include <map>
#include <string>
#include <vector>

#include "s2clientprotocol/sc2api.pb.h"

#include "sc2/api/bridge.hpp"
#include "sc2/api/truth_pack.hpp"
#include "sc2/data/catalog.hpp"

namespace sc2::api::bridge {

bool fill_unit_type_data(Host& host, SC2APIProtocol::ResponseData& out, std::string& error) {
    const UnitTypePack pack = load_unit_type_pack(host.truth_data_dir(), host.data_build());
    if (!pack.loaded) {
        error = pack.error;
        return false;
    }
    std::map<std::string, std::uint32_t> named;
    std::size_t omitted = 0;
    if (const data::Catalog* cat = host.catalog())
        for (const std::string& id : cat->ids("CUnit")) {
            const auto row = pack.id_by_name.find(id);
            if (row != pack.id_by_name.end()) named.emplace(id, row->second);
            else ++omitted;
        }
    std::fprintf(stderr,
                 "api: Data(unit_type_id) published %zu unit type(s); %zu roster entrie(s) had no "
                 "row in %s and were OMITTED rather than given an invented id\n",
                 named.size(), omitted, pack.path.c_str());

    for (const auto& kv : named) {
        SC2APIProtocol::UnitTypeData* d = out.add_units();
        d->set_unit_id(kv.second);
        d->set_name(kv.first);
        d->set_available(true);
    }
    return true;
}

}
