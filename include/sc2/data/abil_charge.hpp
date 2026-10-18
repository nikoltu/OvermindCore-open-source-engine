#pragma once
#include <cstdlib>
#include <string>
#include <vector>

#include "sc2/data/catalog.hpp"

namespace sc2::data {

struct ChargeSpec {
    bool found = false;
    double count_max = 0;
    double count_start = 0;
    double count_use = 0;
    double time_start = 0;
    double time_use = 0;
    std::string link;
    std::string location;
    bool time_queuing = false;
    bool from_attr = false;
    bool from_child = false;
};

struct SlotCharge {
    std::string index;
    ChargeSpec charge;
};

namespace detail {

inline void merge_charge_node(const core::XmlNode& ch, ChargeSpec& out) {
    auto set = [&](const std::string& field, const std::string& value) {
        if (value.empty()) return;
        if (field == "CountMax") out.count_max = std::strtod(value.c_str(), nullptr);
        else if (field == "CountStart") out.count_start = std::strtod(value.c_str(), nullptr);
        else if (field == "CountUse") out.count_use = std::strtod(value.c_str(), nullptr);
        else if (field == "TimeStart") out.time_start = std::strtod(value.c_str(), nullptr);
        else if (field == "TimeUse") out.time_use = std::strtod(value.c_str(), nullptr);
        else if (field == "Link") out.link = value;
        else if (field == "Location") out.location = value;
    };
    out.found = true;
    for (const auto& kv : ch.attrs) {
        if (kv.first == "index") continue;
        out.from_attr = true;
        set(kv.first, kv.second);
    }
    for (const core::XmlNode& f : ch.children) {
        out.from_child = true;
        if (f.name == "Flags") {
            if (f.attr_or("index", "") == "EnableChargeTimeQueuing")
                out.time_queuing = f.attr_or("value", "") != "0";
            continue;
        }
        set(f.name, f.text.empty() ? f.attr_or("value", "") : f.text);
    }
    if (const std::string* fl = ch.attr("Flags"))
        if (fl->find("EnableChargeTimeQueuing") != std::string::npos) out.time_queuing = true;
}

}

inline ChargeSpec ability_cost_charge(const Catalog& cat, const std::string& abil_id) {
    ChargeSpec out;
    for (const core::XmlNode& n : cat.resolved("CAbil", abil_id)) {
        if (n.name != "Cost") continue;
        for (const core::XmlNode& ch : n.children)
            if (ch.name == "Charge") detail::merge_charge_node(ch, out);
    }
    return out;
}

inline ChargeSpec ability_charge(const Catalog& cat, const std::string& abil_id,
                                 const std::string& index) {
    ChargeSpec out;
    for (const core::XmlNode& n : cat.resolved("CAbil", abil_id)) {
        if (n.name != "InfoArray") continue;
        if (n.attr_or("index", "") != index) continue;
        for (const core::XmlNode& ch : n.children)
            if (ch.name == "Charge") detail::merge_charge_node(ch, out);
    }
    return out;
}

struct CooldownSpec {
    bool found = false;
    double time_start = 0;
    double time_use = 0;
    std::string location;
    std::string link;
};
inline CooldownSpec ability_cooldown(const Catalog& cat, const std::string& abil_id, const std::string& index) {
    CooldownSpec out;
    auto set = [&](const std::string& field, const std::string& value) {
        if (value.empty()) return;
        if (field == "TimeStart") out.time_start = std::strtod(value.c_str(), nullptr);
        else if (field == "TimeUse") out.time_use = std::strtod(value.c_str(), nullptr);
        else if (field == "Location") out.location = value;
        else if (field == "Link") out.link = value;
    };
    for (const core::XmlNode& n : cat.resolved("CAbil", abil_id)) {
        if (n.name != "InfoArray" || n.attr_or("index", "") != index) continue;
        for (const core::XmlNode& cd : n.children) {
            if (cd.name != "Cooldown") continue;
            out.found = true;
            for (const auto& kv : cd.attrs) set(kv.first, kv.second);
            for (const core::XmlNode& f : cd.children) set(f.name, f.text.empty() ? f.attr_or("value", "") : f.text);
        }
    }
    return out;
}

inline std::vector<SlotCharge> ability_charges(const Catalog& cat, const std::string& abil_id) {
    std::vector<std::string> order;
    for (const core::XmlNode& n : cat.resolved("CAbil", abil_id)) {
        if (n.name != "InfoArray") continue;
        const std::string idx = n.attr_or("index", "");
        if (idx.empty()) continue;
        bool has_charge = false;
        for (const core::XmlNode& ch : n.children)
            if (ch.name == "Charge") { has_charge = true; break; }
        if (!has_charge) continue;
        bool seen = false;
        for (const std::string& s : order)
            if (s == idx) { seen = true; break; }
        if (!seen) order.push_back(idx);
    }
    std::vector<SlotCharge> out;
    out.reserve(order.size());
    for (const std::string& idx : order) out.push_back({idx, ability_charge(cat, abil_id, idx)});
    return out;
}

}
