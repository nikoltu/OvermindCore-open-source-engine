#pragma once

#include <map>
#include <memory>
#include <set>
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
    const formats::Model* ensure_build(casc::Storage& store, const std::string& type_token);

    const formats::Model* doodad_model(casc::Storage& store, const std::string& doodad_type,
                                       int variation = 0);

    const formats::Model* carry_model(casc::Storage& store, const std::string& node_type);

    const formats::Model* missile_model(casc::Storage& store, const std::string& weapon_id);
    const formats::Model* death_model(casc::Storage& store, const std::string& actor_model_id);
    const formats::Model* scaffold_model(casc::Storage& store, const std::string& actor_model_id, float scale[3]);
    const formats::Model* unit_splat(casc::Storage& store, const std::string& unit_id);
    struct Portrait {
        formats::Model model;
        std::string path;
        float fov_rad = 0.0f;
        int cam_bone = -1;
        float znear = 0.0f;
        float zfar = 0.0f;
        bool ok = false;
    };
    const Portrait* portrait(casc::Storage& store, const std::string& unit_id);

    const std::set<std::string>& doodad_misses() const { return doodad_misses_; }

    std::vector<std::string> resolved() const;

    size_t size() const { return models_.size(); }

    static const std::vector<std::string>& type_tokens();

private:
    void resolve_one(casc::Storage& store, const std::string& token);
    data::Catalog& catalog(casc::Storage& store);

    std::map<std::string, formats::Model> models_;
    std::map<std::string, formats::Model> doodads_;
    std::map<std::string, formats::Model> carries_;
    std::map<std::string, formats::Model> missiles_;
    std::map<std::string, formats::Model> deaths_;
    struct Scaffold {
        formats::Model model;
        float scale[3] = {1.0f, 1.0f, 1.0f};
    };
    std::map<std::string, Scaffold> scaffolds_;
    std::map<std::string, formats::Model> splats_;
    std::map<std::string, Portrait> portraits_;
    std::map<std::string, std::string> splat_actor_by_unit_;
    bool splat_index_built_ = false;
    std::set<std::string> doodad_misses_;
    std::unique_ptr<data::Catalog> catalog_;
    bool catalog_ready_ = false;
};

}
