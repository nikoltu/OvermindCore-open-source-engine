#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "sc2/casc/encoding.hpp"

namespace sc2::casc {

class Mndx {
public:
    void parse(const std::vector<uint8_t>& root_blob);

    const std::vector<std::pair<std::string, CKey>>& enumerate() const {
        return entries_;
    }

    const CKey* resolve(const std::string& path) const;

private:
    std::vector<std::pair<std::string, CKey>> entries_;
    std::unordered_map<std::string, CKey> by_path_;
};

}
