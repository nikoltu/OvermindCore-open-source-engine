#include "omc/data/requirements.hpp"

#include <cstdlib>

namespace omc::data {

using core::XmlNode;

template <class Nodes>
static const XmlNode* child(const Nodes& children,
                            const std::string& name) {
    const XmlNode* last = nullptr;
    for (const XmlNode& c : children)
        if (c.name == name) last = &c;
    return last;
}

template <class Nodes>
static std::string count_attr(const Nodes& children, const char* attr, const char* def) {
    const std::string* v = nullptr;
    for (const XmlNode& c : children)
        if (c.name == "Count")
            if (const std::string* a = c.attr(attr)) v = a;
    return v ? *v : std::string(def);
}

static bool is_int_literal(const std::string& s) {
    if (s.empty()) return false;
    size_t i = (s[0] == '-' || s[0] == '+') ? 1 : 0;
    if (i >= s.size()) return false;
    for (; i < s.size(); ++i)
        if (s[i] < '0' || s[i] > '9') return false;
    return true;
}

static long count_units(const Catalog& cat, const std::string& link,
                        const std::string& state, const TechState& st) {

    auto tally = [&](const std::unordered_map<std::string, int>& m) -> long {
        long total = 0;
        for (const auto& [uid, cnt] : m) {
            if (uid == link) {
                total += cnt;
                continue;
            }
            for (const std::string& a : cat.array_links("CUnit", uid, "TechAliasArray"))
                if (a == link) {
                    total += cnt;
                    break;
                }
        }
        return total;
    };
    const bool at = state.size() >= 6 && state.compare(state.size() - 6, 6, "AtUnit") == 0;
    const bool pending = state.rfind("Queued", 0) == 0 || state.rfind("InProgress", 0) == 0;
    long n = tally(at ? st.at_unit : st.units_complete);
    if (pending) n += tally(at ? st.at_unit_queued : st.units_queued);
    return n;
}

long eval_node(const Catalog& cat, const std::string& node_id, const TechState& st) {
    if (is_int_literal(node_id)) return std::strtol(node_id.c_str(), nullptr, 10);

    const Catalog::Entry* e = cat.find("CRequirement", node_id);
    if (!e) return 0;
    const std::string& tag = e->tag;
    const auto& kids = cat.resolved("CRequirement", node_id);

    auto count_leaf = [&](const std::unordered_map<std::string, int>& m) -> long {
        if (!child(kids, "Count")) return 0;
        const std::string link = count_attr(kids, "Link", "");
        const std::string state = count_attr(kids, "State", "CompleteOnly");
        if (state.size() >= 6 && state.compare(state.size() - 6, 6, "AtUnit") == 0)
            return count_units(cat, link, state, st);
        auto it = m.find(link);
        return it == m.end() ? 0 : it->second;
    };

    auto operands = [&]() { return cat.array_links("CRequirement", node_id, "OperandArray"); };
    auto truthy = [](long v) { return v != 0; };

    if (tag == "CRequirementCountUnit") {
        if (!child(kids, "Count")) return 0;
        return count_units(cat, count_attr(kids, "Link", ""), count_attr(kids, "State", "CompleteOnly"), st);
    }
    if (tag == "CRequirementCountUpgrade") {
        long n = count_leaf(st.upgrades);
        if (child(kids, "Count")) {
            const std::string state = count_attr(kids, "State", "CompleteOnly");
            if (state.rfind("QueuedOrBetter", 0) == 0 || state.rfind("InProgressOrBetter", 0) == 0) {
                auto it = st.upgrades_queued.find(count_attr(kids, "Link", ""));
                if (it != st.upgrades_queued.end()) n += it->second;
            }
        }
        return n;
    }
    if (tag == "CRequirementCountBehavior") return count_leaf(st.behaviors);
    if (tag == "CRequirementConst") {
        const XmlNode* v = child(kids, "Value");
        return v ? std::strtol(v->attr_or("value", "0").c_str(), nullptr, 10) : 0;
    }
    if (tag == "CRequirementAllowUnit") {
        const XmlNode* l = child(kids, "Link");
        return (l && st.allows(l->attr_or("value", ""))) ? 1 : 0;
    }
    if (tag == "CRequirementAnd") {
        for (const std::string& o : operands())
            if (!truthy(eval_node(cat, o, st))) return 0;
        return 1;
    }
    if (tag == "CRequirementOr") {
        for (const std::string& o : operands())
            if (truthy(eval_node(cat, o, st))) return 1;
        return 0;
    }
    if (tag == "CRequirementNot") {
        auto ops = operands();
        return (!ops.empty() && truthy(eval_node(cat, ops[0], st))) ? 0 : 1;
    }
    if (tag == "CRequirementSum") {
        long s = 0;
        for (const std::string& o : operands()) s += eval_node(cat, o, st);
        return s;
    }
    if (tag == "CRequirementEq" || tag == "CRequirementGT" ||
        tag == "CRequirementGTE" || tag == "CRequirementLT") {
        auto ops = operands();
        if (ops.size() < 2) return 0;
        long a = eval_node(cat, ops[0], st), b = eval_node(cat, ops[1], st);
        if (tag == "CRequirementEq") return a == b;
        if (tag == "CRequirementGT") return a > b;
        if (tag == "CRequirementGTE") return a >= b;
        return a < b;
    }
    return 0;
}

RequirementNodeRef requirement_node(const Catalog& cat, const std::string& req_id,
                                    const std::string& index) {
    RequirementNodeRef ref;
    const Catalog::Entry* e = cat.find("CRequirement", req_id);
    if (!e) return ref;
    ref.exists = true;
    const auto& kids = cat.resolved("CRequirement", req_id);

    const XmlNode* chosen = nullptr;
    const XmlNode* first = nullptr;
    for (const XmlNode& c : kids) {
        if (c.name != "NodeArray") continue;
        if (!first) first = &c;
        if (c.attr_or("index", "") == index) chosen = &c;
    }
    const XmlNode* node = chosen ? chosen : first;
    if (!node) return ref;
    ref.has_node = true;
    ref.exact = chosen != nullptr;
    ref.link = node->attr_or("Link", "");
    return ref;
}

bool requirement_hidden(const Catalog& cat, const std::string& req_id, const TechState& st) {
    if (req_id.empty()) return false;
    const RequirementNodeRef ref = requirement_node(cat, req_id, "Show");
    if (!ref.exists || !ref.has_node || !ref.exact) return false;
    if (ref.link.empty()) return false;
    return eval_node(cat, ref.link, st) == 0;
}

bool eval_requirement(const Catalog& cat, const std::string& req_id,
                      const std::string& index, const TechState& st) {
    const RequirementNodeRef ref = requirement_node(cat, req_id, index);
    if (!ref.exists) return true;
    if (!ref.has_node) return true;
    if (ref.link.empty()) return true;
    return eval_node(cat, ref.link, st) != 0;
}

}
