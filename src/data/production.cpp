#include "sc2/data/production.hpp"

#include <cstdlib>

namespace sc2::data {

void Production::build(const Catalog& cat) {
    edges_.clear();
    by_ability_.clear();
    by_product_.clear();

    for (const std::string& id : cat.ids("CAbil")) {
        const Catalog::Entry* e = cat.find("CAbil", id);
        if (!e) continue;
        const std::string& tag = e->tag;
        if (tag != "CAbilBuild" && tag != "CAbilTrain" && tag != "CAbilWarpTrain" &&
            tag != "CAbilMorph" && tag != "CAbilMerge")
            continue;

        for (const Catalog::AbilInfoRow& r : cat.ability_info_rows(id)) {
            if (r.self_macro || r.unit.empty()) continue;
            const ProductInfo pi{id,           r.index,      r.unit,        r.requirement,
                                 r.time,       r.unit_count, r.warp_charge, tag == "CAbilMorph"};
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
