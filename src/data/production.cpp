#include "sc2/data/production.hpp"

#include <cstdlib>

namespace sc2::data {

using core::XmlNode;

static const XmlNode* find_child(const XmlNode& n, const std::string& name) {
    for (const XmlNode& c : n.children)
        if (c.name == name) return &c;
    return nullptr;
}

void Production::build(const Catalog& cat) {
    edges_.clear();
    by_ability_.clear();
    by_product_.clear();

    for (const std::string& id : cat.ids("CAbil")) {
        const Catalog::Entry* e = cat.find("CAbil", id);
        if (!e) continue;
        const std::string& tag = e->tag;
        if (tag != "CAbilBuild" && tag != "CAbilTrain" && tag != "CAbilWarpTrain" &&
            tag != "CAbilMorph")
            continue;

        std::unordered_map<std::string, ProductInfo> by_index;
        std::vector<std::string> order;
        for (const XmlNode& c : cat.resolved("CAbil", id)) {
            if (c.name != "InfoArray") continue;
            std::string index = c.attr_or("index", "");
            std::string product = c.attr_or("Unit", "");
            if (product.empty()) {
                if (const XmlNode* u = find_child(c, "Unit"))
                    product = u->attr_or("value", "");
            }
            std::string req;
            if (const XmlNode* b = find_child(c, "Button"))
                req = b->attr_or("Requirements", "");
            double warp_charge = 0;
            if (tag == "CAbilWarpTrain") {
                if (const XmlNode* ch = find_child(c, "Charge")) {
                    std::string v = ch->attr_or("TimeUse", "");
                    if (v.empty())
                        if (const XmlNode* tu = find_child(*ch, "TimeUse"))
                            v = tu->text.empty() ? tu->attr_or("value", "") : tu->text;
                    if (!v.empty()) warp_charge = std::strtod(v.c_str(), nullptr);
                }
            }
            if (product.find("##") != std::string::npos) continue;
            if (product.empty()) {
                auto it = by_index.find(index);
                if (it != by_index.end() && warp_charge > 0) it->second.warp_charge = warp_charge;
                continue;
            }
            double time = std::strtod(c.attr_or("Time", "0").c_str(), nullptr);
            if (warp_charge == 0) {
                auto it = by_index.find(index);
                if (it != by_index.end()) warp_charge = it->second.warp_charge;
            }
            if (by_index.find(index) == by_index.end()) order.push_back(index);
            by_index[index] = ProductInfo{id, index, product, req, time, warp_charge};
        }
        for (const std::string& idx : order) {
            const ProductInfo& pi = by_index[idx];
            edges_.push_back(pi);
            by_ability_[id].push_back(pi);
            by_product_[pi.product].push_back(pi);
        }
    }
}

static const std::vector<ProductInfo> kEmpty;

const std::vector<ProductInfo>& Production::products_of(
    const std::string& ability) const {
    auto it = by_ability_.find(ability);
    return it == by_ability_.end() ? kEmpty : it->second;
}

const std::vector<ProductInfo>& Production::producers_of(
    const std::string& product) const {
    auto it = by_product_.find(product);
    return it == by_product_.end() ? kEmpty : it->second;
}

bool Production::can_produce(const Catalog& cat, const std::string& ability,
                            const std::string& product,
                            const TechState& st) const {
    auto it = by_ability_.find(ability);
    if (it == by_ability_.end()) return false;
    for (const ProductInfo& pi : it->second) {
        if (pi.product != product) continue;
        if (pi.requirement.empty()) return true;
        return eval_requirement(cat, pi.requirement, "Use", st);
    }
    return false;
}

}
