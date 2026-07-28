#pragma once

#include <string>
#include <unordered_map>

#include "sc2/data/catalog.hpp"

namespace sc2::data {

struct TechState {
    std::unordered_map<std::string, int> units_complete;
    std::unordered_map<std::string, int> units_queued;
    std::unordered_map<std::string, int> upgrades;
    std::unordered_map<std::string, int> behaviors;
    std::unordered_map<std::string, int> at_unit;
    std::unordered_map<std::string, bool> disallowed;

    bool allows(const std::string& link) const {
        auto it = disallowed.find(link);
        return it == disallowed.end() || !it->second;
    }
};

bool eval_requirement(const Catalog& cat, const std::string& req_id,
                      const std::string& index, const TechState& st);

long eval_node(const Catalog& cat, const std::string& node_id, const TechState& st);

}
