#pragma once

#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "sc2/data/environment.hpp"
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
    const formats::Model* doodad_projectors(casc::Storage& store, const std::string& doodad_type,
                                            int variation = 0);
    const formats::Model* doodad_terrain(casc::Storage& store, const std::string& doodad_type, int variation = 0);
    const formats::Model* foliage_model(casc::Storage& store, const std::string& foliage_id, int variation = 0);
    bool doodad_adjusts_terrain(casc::Storage& store, const std::string& doodad_type);
    void set_texture_prop(const std::string& prop) { texture_prop_ = prop; }
    void set_tileset(casc::Storage& store, const std::string& tileset);
    const std::string& texture_prop() const { return texture_prop_; }

    const formats::Model* carry_model(casc::Storage& store, const std::string& node_type);

    const formats::Model* missile_model(casc::Storage& store, const std::string& weapon_id);
    const formats::Model* impact_model(casc::Storage& store, const std::string& weapon_id);
    const formats::Model* launch_model(casc::Storage& store, const std::string& weapon_id);
    struct BeamModel {
        const formats::Model* model = nullptr;
        bool continuous = false;
        float scale = 1.0f;
    };
    BeamModel beam_model(casc::Storage& store, const std::string& weapon_id);
    struct BurnState {
        std::string state;
        const formats::Model* model = nullptr;
    };
    const std::vector<BurnState>& burn_states(casc::Storage& store, const std::string& unit_id);
    const formats::Model* cmodel(casc::Storage& store, const std::string& cmodel_id);
    BeamModel actor_beam_model(casc::Storage& store, const std::string& beam_actor_id);
    const formats::Model* death_model(casc::Storage& store, const std::string& actor_model_id);
    const formats::Model* scaffold_model(casc::Storage& store, const std::string& actor_model_id, float scale[3]);
    const formats::Model* unit_splat(casc::Storage& store, const std::string& unit_id);
    bool unit_splat_from_actor(const std::string& unit_id) const { return actor_created_splats_.count(unit_id) != 0; }
    struct GroundDecal {
        formats::Model model;
        bool has_forward = false;
        float forward[2] = {0.0f, -1.0f};
        float offset[2] = {0.0f, 0.0f};
        float scale = 1.0f;
    };
    const GroundDecal* unit_ground_decal(casc::Storage& store, const std::string& unit_id);
    float unit_splat_scale(const std::string& unit_id) const {
        const auto it = splat_scale_by_unit_.find(unit_id);
        return it == splat_scale_by_unit_.end() ? 1.0f : it->second;
    }
    const formats::Model* actor_splat(casc::Storage& store, const std::string& actor_id);
    struct Portrait {
        formats::Model model;
        std::string path;
        float fov_rad = 0.0f;
        int cam_bone = -1;
        float znear = 0.0f;
        float zfar = 0.0f;
        float dof_focus = 0.0f, dof_start = 0.0f, dof_end = 0.0f, dof_amount = 0.0f;
        data::EnvLight light;
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
    data::Catalog& unit_catalog(casc::Storage& store, const std::string& token, bool& story);
    std::unique_ptr<data::Catalog> story_catalog_;
    bool story_ready_ = false;

    std::map<std::string, formats::Model> models_;
    std::map<std::string, formats::Model> doodads_;
    std::map<std::string, formats::Model> carries_;
    std::map<std::string, formats::Model> missiles_;
    std::map<std::string, formats::Model> impacts_;
    std::map<std::string, formats::Model> launches_;
    struct BeamSlot { formats::Model model; bool continuous = false; float scale = 1.0f; };
    std::map<std::string, BeamSlot> beams_;
    std::map<std::string, std::string> weapon_beam_;
    std::map<std::string, std::vector<BurnState>> burn_;
    std::map<std::string, formats::Model> burn_models_;
    std::map<std::string, formats::Model> deaths_;
    struct Scaffold {
        formats::Model model;
        float scale[3] = {1.0f, 1.0f, 1.0f};
    };
    std::map<std::string, Scaffold> scaffolds_;
    std::map<std::string, formats::Model> splats_;
    std::map<std::string, formats::Model> actor_splats_;
    formats::Model load_splat_model(casc::Storage& store, const std::string& actor_id);
    std::map<std::string, Portrait> portraits_;
    std::map<std::string, std::string> splat_actor_by_unit_;
    std::map<std::string, float> splat_scale_by_unit_;
    std::map<std::string, GroundDecal> ground_decals_;
    std::set<std::string> actor_created_splats_;
    bool splat_index_built_ = false;
    std::set<std::string> doodad_misses_;
    std::string texture_prop_;
    std::set<std::string> unit_prop_done_;

public:
    const formats::Model* ensure_variation(casc::Storage& store, const std::string& token, unsigned pick);

private:
    std::string doodad_key(const std::string& type, int variation) const {
        std::string k = variation > 0 ? type + '#' + std::to_string(variation) : type;
        if (!texture_prop_.empty()) k += '@' + texture_prop_;
        return k;
    }
    std::unique_ptr<data::Catalog> catalog_;
    bool catalog_ready_ = false;
};

}
