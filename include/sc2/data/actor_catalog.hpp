#pragma once

#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace sc2::data {

class Catalog;

struct ActorRule {
    std::string terms;
    std::string target;
    std::string send;

    std::string event;
    std::string event_head;
    std::vector<std::string> filters;
    std::string verb;
    std::vector<std::string> args;
};

struct ActorDeath {
    std::string index;
    std::string anim_props;
    std::string model_link;
    std::string sound_link;
};

struct CActorUnit {
    std::string id;
    std::string unit_name;
    std::string parent;
    std::string model;
    std::optional<double> walk_anim_move_speed;
    std::vector<std::pair<std::string, std::string>> baselines;
    std::vector<ActorDeath> death_array;
    std::vector<ActorDeath> death_customs;
    std::vector<std::string> macros;
    std::vector<ActorRule> on;
};

struct CActorEventMacro {
    std::string id;
    std::string parent;
    std::vector<ActorRule> on;
};

struct CModel {
    std::string id;
    std::string parent;
    std::string model;
    struct Event {
        std::string name;
        std::string anim;
        std::string time;
    };
    std::vector<Event> events;
};

class ActorCatalog {
public:
    void build(const Catalog& catalog);

    const CActorUnit* actor_unit(const std::string& id) const;
    const CActorUnit* actor_for_unit(const std::string& unit_name) const;
    const CActorEventMacro* event_macro(const std::string& id) const;
    const CModel* model(const std::string& id) const;

    std::vector<std::string> actor_unit_ids() const;
    size_t actor_unit_count() const { return actor_units_.size(); }

private:
    std::unordered_map<std::string, CActorUnit> actor_units_;
    std::unordered_map<std::string, std::string> by_unit_name_;
    std::unordered_map<std::string, CActorEventMacro> macros_;
    std::unordered_map<std::string, CModel> models_;
};

}
