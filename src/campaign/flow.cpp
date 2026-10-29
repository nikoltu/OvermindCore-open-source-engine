#include "omc/campaign/flow.hpp"

#include "omc/data/strings.hpp"

#include <sstream>

namespace omc::campaign {

static const char kFlowData[] = R"FLOW(
campaign WOL text=UI/ExpansionLevel/WingsOfLiberty
node TRaynor01 always
node TRaynor02 req=TRaynor01
node TRaynor03 req=TRaynor02
node THanson01 req=TRaynor03
node THanson02 req=THanson01
node THanson03A req=THanson02 gate=12
node THanson03B req=THanson02 gate=12
node TTosh01 gate=4
node TTosh02 req=TTosh01
node TTosh03A req=TTosh02 gate=13
node TTosh03B req=TTosh02 gate=13
node THorner01 gate=6
node THorner02 req=THorner01
node THorner03 req=THorner02
node THorner04 req=THorner03
node THorner05S req=THorner04
node TTychus01 req=TRaynor03
node TTychus02 req=TTychus01 gate=8
node TTychus03 req=TTychus02 gate=11
node TTychus04 req=TTychus03 gate=14
node TTychus05 req=TTychus04
node TZeratul01 req=TTychus02
node TZeratul02 req=TZeratul01
node TZeratul03 req=TZeratul02
node TZeratul04 req=TZeratul03
node TValerian01 req=TTychus05
node TValerian02A req=TValerian01
node TValerian02B req=TValerian01
node TValerian03 reqany=TValerian02A,TValerian02B
branch THanson03A THanson03B
branch TTosh03A TTosh03B
branch TValerian02A TValerian02B

campaign HOTS text=UI/ExpansionLevel/HeartOfTheSwarm
node ZLab1 always
node ZLab2 req=ZLab1
node ZLab3 req=ZLab2
node ZExpedition1 req=ZLab3
node ZExpedition2 req=ZExpedition1
node ZExpedition3 req=ZExpedition2
node ZChar1 req=ZLab3
node ZChar2 req=ZChar1
node ZChar3 req=ZChar2
node ZZerus1 arc=2
node ZZerus2 req=ZZerus1
node ZZerus3 req=ZZerus2
node ZHybrid1 arc=4
node ZHybrid2 req=ZHybrid1
node ZHybrid3 req=ZHybrid2
node ZSpace1 arc=4
node ZSpace2 req=ZSpace1
node ZKorhal1 arc=6
node ZKorhal2 req=ZKorhal1
node ZKorhal3 req=ZKorhal2
arc ZLab3
arc ZExpedition3
arc ZChar3
arc ZZerus3
arc ZHybrid3
arc ZSpace2
arc ZKorhal3

campaign LOTV text=UI/ExpansionLevel/LegacyOfTheVoid
node PAiur01 always
node PAiur02 req=PAiur01
node PAiur03 req=PAiur02
node PKorhal01 req=PAiur03
node PKorhal02 req=PKorhal01
node PShakuras01 req=PAiur03
node PShakuras02 req=PShakuras01
node PPurifier01 arc=2
node PUlnar01 arc=4
node PUlnar02 req=PUlnar01
node PUlnar03 req=PUlnar02
node PPurifier02 req=PUlnar03
node PPurifier03 req=PPurifier02
node PTaldarim01 req=PUlnar03
node PTaldarim02 req=PTaldarim01
node PMoebius01 arc=6
node PAiur04 arc=8
node PAiur05 req=PAiur04
node PAiur06 req=PAiur05
node SC2Epilogue01 arc=9
node SC2Epilogue02 req=SC2Epilogue01
node SC2Epilogue03 req=SC2Epilogue02
arc PAiur03
arc PKorhal02
arc PPurifier01
arc PPurifier03
arc PShakuras02
arc PTaldarim02
arc PMoebius01
arc PUlnar03
arc PAiur06
)FLOW";

const std::string& campaign_flow_data() {
    static const std::string data(kFlowData);
    return data;
}

static std::vector<std::string> split_csv(const std::string& s) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == ',') {
            if (!cur.empty()) out.push_back(cur);
            cur.clear();
        } else {
            cur.push_back(c);
        }
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

std::vector<CampaignFlow> parse_flow(const std::string& text) {
    std::vector<CampaignFlow> flows;
    CampaignFlow* cur = nullptr;

    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        std::istringstream ls(line);
        std::string tok;
        if (!(ls >> tok)) continue;
        if (tok[0] == '#') continue;

        if (tok == "campaign") {
            flows.push_back(CampaignFlow{});
            cur = &flows.back();
            ls >> cur->name;
            std::string kv;
            while (ls >> kv)
                if (kv.rfind("text=", 0) == 0) cur->title_key = kv.substr(5);
        } else if (tok == "node" && cur) {
            MissionNode n;
            ls >> n.id;
            std::string kv;
            while (ls >> kv) {
                if (kv == "always") {
                    n.always = true;
                } else if (kv.rfind("req=", 0) == 0) {
                    n.prereqs = split_csv(kv.substr(4));
                } else if (kv.rfind("reqany=", 0) == 0) {
                    n.prereqs = split_csv(kv.substr(7));
                    n.prereqs_any = true;
                } else if (kv.rfind("gate=", 0) == 0) {
                    n.gate_count = std::stoi(kv.substr(5));
                } else if (kv.rfind("arc=", 0) == 0) {
                    n.arc_gate = std::stoi(kv.substr(4));
                }
            }
            cur->index[n.id] = cur->nodes.size();
            cur->nodes.push_back(std::move(n));
        } else if (tok == "arc" && cur) {
            std::string id;
            if (ls >> id) cur->arc_terminals.push_back(id);
        } else if (tok == "branch" && cur) {
            std::vector<MissionId> group;
            std::string id;
            while (ls >> id) group.push_back(id);
            if (group.size() >= 2) cur->branches.push_back(std::move(group));
        }
    }
    return flows;
}

std::vector<CampaignFlow> load_campaign_flow() {
    return parse_flow(campaign_flow_data());
}

std::string campaign_title(const std::vector<CampaignFlow>& flows, std::size_t i, const data::StringTable& text) {
    if (i >= flows.size()) return {};
    if (!flows[i].title_key.empty())
        if (const std::string* s = text.find(flows[i].title_key); s && !s->empty()) return *s;
    return "Campaign " + std::to_string(i + 1);
}

const MissionNode* CampaignFlow::find(const MissionId& id) const {
    auto it = index.find(id);
    if (it == index.end()) return nullptr;
    return &nodes[it->second];
}

std::size_t CampaignFlow::count_completed(const CompletedSet& completed) const {
    std::size_t n = 0;
    for (const MissionId& m : completed)
        if (index.find(m) != index.end()) ++n;
    return n;
}

int32_t CampaignFlow::arcs_completed(const CompletedSet& completed) const {
    int32_t n = 0;
    for (const MissionId& t : arc_terminals)
        if (completed.count(t)) ++n;
    return n;
}

bool CampaignFlow::locked_by_branch_sibling(const MissionId& id,
                                            const CompletedSet& completed) const {
    for (const auto& group : branches) {
        bool in_group = false;
        for (const MissionId& m : group)
            if (m == id) { in_group = true; break; }
        if (!in_group) continue;
        for (const MissionId& m : group)
            if (m != id && completed.count(m)) return true;
    }
    return false;
}

bool CampaignFlow::available(const MissionId& id,
                             const CompletedSet& completed) const {
    const MissionNode* n = find(id);
    if (!n) return false;

    if (!n->prereqs.empty()) {
        if (n->prereqs_any) {
            bool any = false;
            for (const MissionId& r : n->prereqs)
                if (completed.count(r)) { any = true; break; }
            if (!any) return false;
        } else {
            for (const MissionId& r : n->prereqs)
                if (!completed.count(r)) return false;
        }
    }

    if (n->gate_count > 0 &&
        static_cast<int32_t>(count_completed(completed)) < n->gate_count)
        return false;

    if (n->arc_gate > 0 && arcs_completed(completed) < n->arc_gate) return false;

    if (locked_by_branch_sibling(id, completed)) return false;

    return true;
}

std::vector<MissionId> CampaignFlow::available_missions(
    const CompletedSet& completed) const {
    std::vector<MissionId> out;
    for (const MissionNode& n : nodes)
        if (!completed.count(n.id) && available(n.id, completed))
            out.push_back(n.id);
    return out;
}

}
