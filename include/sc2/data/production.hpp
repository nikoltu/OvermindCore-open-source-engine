#pragma once

#include <string>
#include <unordered_map>
#include <vector>

#include "sc2/data/catalog.hpp"
#include "sc2/data/requirements.hpp"

namespace sc2::data {

struct ProductInfo {
    std::string ability;
    std::string index;
    std::string product;
    std::string requirement;
    double time = 0;
};

class Production {
public:
    void build(const Catalog& cat);

    const std::vector<ProductInfo>& products_of(const std::string& ability) const;
    const std::vector<ProductInfo>& producers_of(const std::string& product) const;

    bool can_produce(const Catalog& cat, const std::string& ability,
                     const std::string& product, const TechState& st) const;

    size_t edge_count() const { return edges_.size(); }

private:
    std::vector<ProductInfo> edges_;
    std::unordered_map<std::string, std::vector<ProductInfo>> by_ability_;
    std::unordered_map<std::string, std::vector<ProductInfo>> by_product_;
};

}
