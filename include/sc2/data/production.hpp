#pragma once

#include <cstdlib>
#include <string>
#include <unordered_map>
#include <vector>

#include "sc2/data/catalog.hpp"
#include "sc2/data/requirements.hpp"
#include "sc2/sim/fixed.hpp"

namespace sc2::data {

#include "sc2/data/authored.hpp"

struct ProductInfo {
    std::string ability;
    std::string index;
    std::string product;
    std::string requirement;
    double time = 0;
    int count = 1;
    double warp_charge = 0;
    bool from_morph = false;
};

class Production {
public:
    void build(const Catalog& cat);

    const std::vector<ProductInfo>& products_of(const std::string& ability) const;
    const std::vector<ProductInfo>& producers_of(const std::string& product) const;

    double time_of(const std::string& product) const {
        auto it = by_product_.find(product);
        if (it == by_product_.end() || it->second.empty()) return 0.0;
        for (const ProductInfo& pi : it->second)
            if (!pi.from_morph) return pi.time;
        return it->second.front().time;
    }

    int train_count(const std::string& product) const {
        auto it = by_product_.find(product);
        if (it == by_product_.end()) return 1;
        int c = 1;
        for (const ProductInfo& pi : it->second)
            if (!pi.from_morph && pi.count > c) c = pi.count;
        return c;
    }

    double warp_charge_time(const std::string& product) const {
        auto it = by_product_.find(product);
        if (it == by_product_.end()) return 0.0;
        for (const ProductInfo& pi : it->second)
            if (pi.warp_charge > 0) return pi.warp_charge;
        return 0.0;
    }

    bool can_produce(const Catalog& cat, const std::string& ability,
                     const std::string& product, const TechState& st) const;

    size_t edge_count() const { return edges_.size(); }

private:
    std::vector<ProductInfo> edges_;
    std::unordered_map<std::string, std::vector<ProductInfo>> by_ability_;
    std::unordered_map<std::string, std::vector<ProductInfo>> by_product_;
};

}
