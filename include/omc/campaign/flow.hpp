#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace omc::casc {
class Storage;
}

namespace omc::data {
class StringTable;
}

namespace omc::campaign {

using MissionId = std::string;
using CompletedSet = std::set<MissionId>;

struct MissionNode {
    MissionId id;
    std::vector<MissionId> prereqs;
    bool prereqs_any = false;
    int32_t gate_count = 0;
    int32_t arc_gate = 0;
    bool always = false;
    std::vector<std::string> unmodelled;
};

struct CampaignFlow {
    std::string name;
    std::string title_stem;
    std::vector<MissionNode> nodes;
    std::map<MissionId, std::size_t> index;

    std::vector<MissionId> arc_terminals;

    std::vector<std::vector<MissionId>> branches;

    const MissionNode* find(const MissionId& id) const;

    std::size_t count_completed(const CompletedSet& completed) const;
    int32_t arcs_completed(const CompletedSet& completed) const;
    bool locked_by_branch_sibling(const MissionId& id,
                                  const CompletedSet& completed) const;

    bool available(const MissionId& id, const CompletedSet& completed) const;

    std::vector<MissionId> available_missions(const CompletedSet& completed) const;

    std::size_t edge_count() const { return nodes.size(); }
};

CampaignFlow derive_flow(const std::string& name, const std::vector<std::string>& scripts);

std::vector<CampaignFlow> load_campaign_flow(const casc::Storage& store);

std::string campaign_title(const std::vector<CampaignFlow>& flows, std::size_t i, const data::StringTable& text);

}
