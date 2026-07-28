#include "sc2/render/actor_rules.hpp"

#include "sc2/data/actor_catalog.hpp"

namespace sc2::render {

namespace {

std::vector<std::string> split_props(const std::string& s) {
    std::vector<std::string> out;
    size_t start = 0;
    for (size_t i = 0; i <= s.size(); ++i) {
        if (i == s.size() || s[i] == ',') {
            size_t a = start, b = i;
            while (a < b && (s[a] == ' ' || s[a] == '\t')) ++a;
            while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t')) --b;
            if (b > a) out.push_back(s.substr(a, b - a));
            start = i + 1;
        }
    }
    return out;
}

std::string anim_props_of(const data::ActorRule& r) {
    if (r.args.size() >= 2) return r.args[1];
    if (r.args.size() == 1) return r.args[0];
    return {};
}

}

ActorRules ActorRules::from_actor(const data::CActorUnit& a) {
    ActorRules out;
    out.valid = true;

    for (const data::ActorRule& r : a.on) {
        if (r.event_head == "UnitBirth" && r.verb == "AnimPlay") {
            std::vector<std::string> t = split_props(anim_props_of(r));
            if (!t.empty()) { out.by_state[int(AnimState::Birth)] = std::move(t); break; }
        }
    }

    const data::ActorRule* atk = nullptr;
    for (const data::ActorRule& r : a.on) {
        if (r.event_head == "WeaponStart" && r.verb == "AnimBracketStart") {
            if (!atk) atk = &r;
            if (r.filters.empty()) { atk = &r; break; }
        }
    }
    if (atk) {
        std::vector<std::string> t = split_props(anim_props_of(*atk));
        if (!t.empty()) out.by_state[int(AnimState::Attack)] = std::move(t);
    }

    for (const data::ActorDeath& d : a.death_array) {
        if (d.index == "Normal") {
            std::vector<std::string> t = split_props(d.anim_props);
            if (!t.empty()) out.by_state[int(AnimState::Death)] = std::move(t);
            break;
        }
    }

    for (const std::pair<std::string, std::string>& b : a.baselines) {
        const std::string& idx = b.first;
        std::vector<std::string> t = split_props(b.second);
        if (t.empty()) t = {idx};
        if (idx == "Stand") out.by_state[int(AnimState::Stand)] = std::move(t);
        else if (idx == "Walk") out.by_state[int(AnimState::Walk)] = std::move(t);
    }

    static const char* const kDefault[5] = {"Birth", "Stand", "Walk", "Attack", "Death"};
    for (int i = 0; i < 5; ++i)
        if (out.by_state[i].empty()) out.by_state[i] = {kDefault[i]};

    return out;
}

}
