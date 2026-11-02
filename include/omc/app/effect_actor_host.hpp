#pragma once
#include <cstdint>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include "omc/formats/m3.hpp"
#include "omc/render/actor.hpp"
#include "omc/sim/world.hpp"

namespace omc::data {
class Catalog;
}

namespace omc::app {

struct EffectActor {
    std::string actor;
    std::string family;
    std::string head, subject;
    std::string site;
    std::uint64_t instance = 0;
    std::string site_op;
    std::uint32_t unit = sim::kNoTarget;
    bool stationary = false;
    bool from_field = false;
    bool dying = false;
    float x = 0.0f, y = 0.0f;
    float scale[3] = {1.0f, 1.0f, 1.0f};
    float base_scale[3] = {1.0f, 1.0f, 1.0f};
    int player = -1;
    std::string cmodel;
    const formats::Model* model = nullptr;
    render::ActorInstance inst;
    int seq = -1;
    float time_ms = 0.0f;
    float age_ms = 0.0f;
    float orphan_ms = -1.0f;
    std::uint64_t field_key = 0;
    float field_on_s = -1.0f;
    std::string field_on_behavior;
    std::string death_model;
    std::uint32_t caster = sim::kNoTarget, target = sim::kNoTarget;
    float auto_scale = 0.0f;
    bool inherit_scale = false;
    bool held = false;
    bool beam() const { return family == "CActorBeamSimple"; }
    bool overlay() const { return family == "CActorModelMaterial" || family == "CActorSimple"; }
    bool closing() const {
        for (const render::AnimSlot& s : inst.slots)
            if (s.closing_phase) return true;
        return false;
    }
    struct ModelEvt {
        std::string anim, name;
        float time_ms = 0.0f;
    };
    std::vector<ModelEvt> model_evts;
};

struct UnitGlaze {
    std::string cmodel;
    std::string owner;
    const formats::Model* model = nullptr;
    int seq = -1;
    float time_ms = 0.0f;
};

struct UnitLook {
    render::ActorInstance bus;
    std::vector<UnitGlaze> glazes;
};

class EffectActorHost {
public:
    static constexpr float kOrphanMs = 10000.0f;
    using ModelLookup = std::function<const formats::Model*(const std::string& cmodel_id)>;
    using SoundCue = std::function<void(const std::string& sound_id)>;
    using UnitSend = std::function<void(std::uint32_t unit, const std::string& verb, const std::vector<std::string>& args)>;

    void bind(const data::Catalog* cat, ModelLookup models, SoundCue sound = {});
    void set_unit_send(UnitSend send) { unit_send_ = std::move(send); }
    bool bound() const { return cat_ != nullptr; }
    void observe(const sim::World& w, float dt_ms, bool ticked = true);
    const UnitLook* look(std::uint32_t unit) const {
        if (looks_.empty()) return nullptr;
        auto it = looks_.find(unit);
        return it == looks_.end() ? nullptr : &it->second;
    }

    const std::vector<EffectActor>& actors() const { return actors_; }
    std::size_t count(const std::string& actor) const;
    const EffectActor* find(const std::string& actor) const;
    const std::vector<std::string>& sounds() const { return sounds_; }
    std::size_t created_total() const { return created_total_; }
    std::size_t destroyed_total() const { return destroyed_total_; }
    std::size_t skipped_filters() const { return skipped_filters_; }
    std::size_t skipped_families() const { return skipped_families_; }

private:
    struct Edge {
        std::string head, subject, sub;
        std::uint64_t instance = 0;
        std::uint32_t caster = sim::kNoTarget, target = sim::kNoTarget;
        float src_x = 0, src_y = 0, tx = 0, ty = 0;
        int player = -1;
        std::uint32_t on_unit = sim::kNoTarget;
    };
    void edge(const sim::World& w, const Edge& e);
    void create(const sim::World& w, const Edge& e, const std::string& actor, const std::string& family,
                const std::string& site);
    void create_field_actor(const sim::World& w, const sim::ForceFieldInstance& ff, std::uint64_t key);
    bool resolve_model(EffectActor& a, const std::string& cmodel_override);
    void field_life_rules(EffectActor& a, const std::string& unit);
    const std::string& jump_behavior(const std::string& unit);
    void watch_jumps(const sim::World& w);
    std::vector<render::ActorEventRule> rules_of(const std::string& actor, const std::string& family) const;
    void cue_sound_actor(const std::string& actor);
    const std::string& name_of(std::unordered_map<std::uint32_t, std::string>& map, const char* family,
                               std::uint32_t tag) const;
    void model_events(EffectActor& a, int seq_before, float t_before);
    void raise_on_unit(const sim::World& w, const Edge& e, std::uint32_t unit, const char* at);
    void deliver(const sim::World& w, const Edge& e, std::uint32_t unit, const std::string& from_actor,
                 const std::string& from_cmodel, std::vector<render::ActorEventRule> sends);
    const std::vector<render::ActorEventRule>& unit_rules(const std::string& catalog_id);
    std::string family_of(const std::string& actor) const;
    const std::string& channel_abil(const std::string& effect);
    void sync_glazes();

    bool visuals_ = true;
    bool field_life_ = true;
    bool jump_look_ = true;
    struct BodyLife {
        std::uint32_t unit = sim::kNoTarget;
        float on_s = -1.0f;
        std::string behavior;
    };
    std::map<std::uint64_t, BodyLife> body_life_;
    std::unordered_map<std::uint32_t, int> jumps_;
    std::unordered_map<std::string, std::string> jump_behaviors_;
    std::vector<float> last_xy_;
    UnitSend unit_send_;
    std::unordered_map<std::uint32_t, UnitLook> looks_;
    std::unordered_map<std::string, std::vector<render::ActorEventRule>> unit_rules_;
    std::unordered_map<std::string, std::string> channel_abils_;
    bool channel_built_ = false;
    const data::Catalog* cat_ = nullptr;
    ModelLookup models_;
    SoundCue sound_;
    std::vector<EffectActor> actors_;
    std::uint64_t last_seq_ = 0;
    std::map<std::uint32_t, std::set<std::uint32_t>> held_;
    std::map<std::uint64_t, bool> fields_;
    mutable std::unordered_map<std::uint32_t, std::string> effect_names_, behavior_names_, unit_names_, abil_names_;
    mutable std::unordered_map<std::string, std::string> unit_actor_;
    std::vector<std::string> sounds_;
    std::size_t created_total_ = 0, destroyed_total_ = 0, skipped_filters_ = 0, skipped_families_ = 0;
};

}
