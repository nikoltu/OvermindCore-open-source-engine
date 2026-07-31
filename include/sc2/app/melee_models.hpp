#pragma once

#include <map>
#include <memory>
#include <string>
#include <vector>

#include "sc2/formats/m3.hpp"

namespace sc2::casc {
class Storage;
}
namespace sc2::data {
class Catalog;
}

namespace sc2::app {

class MeleeModels {
public:
    MeleeModels();
    ~MeleeModels();
    void load(casc::Storage& store);

    const formats::Model* model_for(const std::string& type_token) const;

    const formats::Model* ensure(casc::Storage& store, const std::string& type_token);

    std::vector<std::string> resolved() const;

    size_t size() const { return models_.size(); }

    static const std::vector<std::string>& type_tokens();

private:
    void resolve_one(casc::Storage& store, const std::string& token);
    data::Catalog& catalog(casc::Storage& store);

    std::map<std::string, formats::Model> models_;
    std::unique_ptr<data::Catalog> catalog_;
    bool catalog_ready_ = false;
};

}
