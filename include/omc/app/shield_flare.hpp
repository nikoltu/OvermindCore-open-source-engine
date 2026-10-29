#pragma once
#include <algorithm>
#include <cstdlib>
#include <optional>
#include <string>
#include <vector>

#include "omc/data/catalog.hpp"
#include "omc/render/actor_rules.hpp"

namespace omc::app {

struct ShieldFlareSpec {
    std::string facer;
    std::string model;
    std::vector<std::string> clip;
    bool destroy_on_clip_end = false;
    float damage_min = 0.0f, damage_max = 0.0f;
    float radius_min = 0.0f, radius_max = 0.0f;
    std::string color_macro;
    std::string ripple;
    std::string ripple_model;
    std::vector<std::string> ripple_clip;
    bool melee_tint = false;
    bool valid() const { return !model.empty() && !clip.empty(); }
};

inline float shield_flare_num(const data::Catalog& cat, const std::string& fam, const std::string& id, const char* field) {
    const std::optional<std::string> v = cat.scalar(fam, id, field);
    return v && !v->empty() ? float(std::atof(v->c_str())) : 0.0f;
}

inline ShieldFlareSpec shield_flare_spec(const data::Catalog& cat, const std::string& impact_actor = "ShieldImpact") {
    ShieldFlareSpec s;
    const std::optional<std::string> facer = cat.scalar("CActorShieldImpact", impact_actor, "VisualDirectionalFacer");
    if (!facer || facer->empty()) return s;
    s.facer = *facer;
    if (const std::optional<std::string> m = cat.scalar("CActorShield", s.facer, "Model")) s.model = *m;
    s.damage_min = shield_flare_num(cat, "CActorShield", s.facer, "ScaleDamageMin");
    s.damage_max = shield_flare_num(cat, "CActorShield", s.facer, "ScaleDamageMax");
    s.radius_min = shield_flare_num(cat, "CActorShield", s.facer, "RadiusMin");
    s.radius_max = shield_flare_num(cat, "CActorShield", s.facer, "RadiusMax");
    if (const std::optional<std::string> mc = cat.scalar("CActorShield", s.facer, "Macros")) s.color_macro = *mc;
    if (const std::optional<std::string> rp = cat.scalar("CActorShield", s.facer, "Ripple"); rp && !rp->empty()) {
        s.ripple = *rp;
        if (const std::optional<std::string> rm = cat.scalar("CActorModel", s.ripple, "Model")) s.ripple_model = *rm;
        for (const data::Catalog::ActorRule& r : cat.actor_rules(s.ripple, "CActorModel"))
            if (r.head == "ActorCreation" && (r.verb == "AnimPlay" || r.verb == "AnimBracketStart") && s.ripple_clip.empty()) {
                const std::vector<std::string> props = render::anim_args_of(r.args).props;
                if (!props.empty()) s.ripple_clip = {props.front()};
            }
    }
    for (const data::Catalog::ActorRule& r : cat.actor_rules(s.facer, "CActorShield")) {
        if (r.head == "ActorCreation" && r.verb == "AnimPlay" && s.clip.empty()) {
            bool scoped = false;
            for (const auto& f : r.filters) scoped = scoped || f.first == "ScopeContains";
            if (!scoped) s.clip = render::anim_args_of(r.args).props;
        }
        if (r.head == "AnimDone" && r.verb == "Destroy")
            for (const auto& f : r.filters)
                if (f.first == "AnimName" && !s.clip.empty() && f.second == s.clip.front()) s.destroy_on_clip_end = true;
        if (r.head == "ActorCreation" && r.verb == "SetTintColor") {
            bool scoped = false;
            for (const auto& f : r.filters) scoped = scoped || (f.first == "ScopeContains" && f.second.rfind("_Shield", 0) == 0);
            if (!scoped) s.melee_tint = true;
        }
    }
    return s;
}

inline float shield_flare_radius(const ShieldFlareSpec& s, float damage) {
    if (s.damage_max <= s.damage_min) return s.radius_min;
    const float k = std::min(1.0f, std::max(0.0f, (damage - s.damage_min) / (s.damage_max - s.damage_min)));
    return s.radius_min + (s.radius_max - s.radius_min) * k;
}

}
