#include "omc/app/effect_actor_host.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <iterator>

#include "omc/app/ability.hpp"
#include "omc/data/actor_model.hpp"
#include "omc/data/catalog.hpp"
#include "omc/render/anim.hpp"

namespace omc::app {

namespace {

std::string field_attr(const data::Catalog& cat, const std::string& family, const std::string& id, const char* field,
                       const char* attr) {
    std::string v;
    for (const core::XmlNode& n : cat.resolved(family, id))
        if (n.name == field)
            if (const std::string* a = n.attr(attr)) v = *a;
    return v;
}

void read_scale3(const std::string& raw, float out[3]) {
    out[0] = out[1] = out[2] = 1.0f;
    if (raw.empty()) return;
    float v[3] = {1.0f, 1.0f, 1.0f};
    const int n = std::sscanf(raw.c_str(), "%f,%f,%f", &v[0], &v[1], &v[2]);
    if (n <= 0) return;
    if (n == 1) v[1] = v[2] = v[0];
    for (int k = 0; k < 3; ++k) out[k] = v[k];
}

bool unit_ok(const sim::World& w, std::uint32_t id) { return id < w.units().size() && w.units()[id].alive; }

std::uint64_t field_key_of(const sim::ForceFieldInstance& ff) {
    return (std::uint64_t(std::uint32_t(ff.x.raw())) << 32) ^ std::uint64_t(std::uint32_t(ff.y.raw())) ^
           (std::uint64_t(ff.unit_tag) << 16);
}

}

void EffectActorHost::bind(const data::Catalog* cat, ModelLookup models, SoundCue sound) {
    cat_ = cat;
    models_ = std::move(models);
    sound_ = std::move(sound);
    visuals_ = std::getenv("OMC_ACTOR_VISUALS_OLD") == nullptr;
    field_life_ = std::getenv("OMC_FIELD_LIFE_OLD") == nullptr;
    jump_look_ = std::getenv("OMC_JUMP_LOOK_OLD") == nullptr;
}

namespace {

double vital_fraction_below(const data::Catalog& cat, const std::string& validator) {
    const data::Catalog::Entry* ve = cat.find("CValidator", validator);
    if (!ve || ve->tag != "CValidatorUnitCompareVital") return -1.0;
    std::string cmp, type, vital = "Life";
    double v = -1.0;
    for (const core::XmlNode& n : cat.resolved("CValidator", validator)) {
        if (n.name == "Compare") cmp = n.attr_or("value", "");
        else if (n.name == "Type") type = n.attr_or("value", "");
        else if (n.name == "Vital") vital = n.attr_or("value", "");
        else if (n.name == "Value") v = std::atof(n.attr_or("value", "-1").c_str());
    }
    return (cmp == "LT" && type == "Fraction" && vital == "Life") ? v : -1.0;
}

}

const std::string& EffectActorHost::jump_behavior(const std::string& unit) {
    auto it = jump_behaviors_.find(unit);
    if (it != jump_behaviors_.end()) return it->second;
    std::string found;
    for (const std::string& b : cat_->array_links("CUnit", unit, "BehaviorArray")) {
        const data::Catalog::Entry* e = b.empty() ? nullptr : cat_->find("CBehavior", b);
        if (e && e->tag == "CBehaviorJump") {
            found = b;
            break;
        }
    }
    return jump_behaviors_.emplace(unit, found).first->second;
}

void EffectActorHost::watch_jumps(const sim::World& w) {
    const sim::NavGrid& nav = w.nav();
    if (!nav.has_levels()) return;
    for (const sim::Unit& u : w.units()) {
        auto jit = jumps_.find(u.id);
        if (!u.alive || !u.can_jump || u.flying || u.catalog_id.empty()) {
            if (jit != jumps_.end()) jumps_.erase(jit);
            continue;
        }
        if (std::size_t(u.id) * 2 + 1 >= last_xy_.size()) continue;
        const std::string& beh = jump_behavior(u.catalog_id);
        if (beh.empty()) continue;
        int px, py, cx, cy;
        nav.to_cell(sim::Fixed::from_double(last_xy_[std::size_t(u.id) * 2]), sim::Fixed::from_double(last_xy_[std::size_t(u.id) * 2 + 1]), px, py);
        nav.to_cell(u.x, u.y, cx, cy);
        if (px == cx && py == cy) continue;
        Edge e;
        e.head = "Behavior";
        e.subject = beh;
        e.on_unit = e.caster = e.target = u.id;
        e.src_x = e.tx = float(u.x.to_double());
        e.src_y = e.ty = float(u.y.to_double());
        e.player = u.player;
        const bool cliff = nav.is_cliff_cell(cx, cy);
        const bool leap = nav.is_leap_edge(px, py, cx, cy);
        if (jit == jumps_.end()) {
            if (!cliff && !leap) continue;
            jit = jumps_.emplace(u.id, int(nav.level_cell(px, py))).first;
            e.sub = "Launch";
            edge(w, e);
            if (cliff) continue;
        } else if (cliff) {
            continue;
        }
        const int from = jit->second;
        jumps_.erase(jit);
        e.sub = int(nav.level_cell(cx, cy)) > from ? "LandUp" : "LandDown";
        edge(w, e);
    }
}

void EffectActorHost::field_life_rules(EffectActor& a, const std::string& unit) {
    double life_max = cat_->scalar_num("CUnit", unit, "LifeMax").value_or(0.0);
    double period = 0.0, change = 0.0;
    std::string below_beh;
    double below = -1.0;
    for (const core::XmlNode& n : cat_->resolved("CUnit", unit)) {
        if (n.name != "BehaviorArray") continue;
        const std::string b = n.attr_or("Link", "");
        if (b.empty()) continue;
        double p = 0.0;
        std::string eff;
        for (const core::XmlNode& c : cat_->resolved("CBehavior", b)) {
            if (c.name == "Period") p = std::atof(c.attr_or("value", "0").c_str());
            else if (c.name == "PeriodicEffect") eff = c.attr_or("value", "");
            else if (c.name == "DisableValidatorArray") {
                const double f = vital_fraction_below(*cat_, c.attr_or("value", ""));
                if (f > 0.0) {
                    below = f;
                    below_beh = b;
                }
            }
        }
        if (p > 0.0 && !eff.empty()) {
            for (const core::XmlNode& c : cat_->resolved("CEffect", eff))
                if (c.name == "VitalArray" && c.attr_or("index", "") == "Life")
                    for (const core::XmlNode& g : c.children)
                        if (g.name == "Change") {
                            period = p;
                            change = std::atof(g.attr_or("value", "0").c_str());
                        }
        }
    }
    if (life_max > 0.0 && period > 0.0 && change < 0.0 && below > 0.0) {
        const double goal = below * life_max;
        int ticks = 0;
        double life = life_max;
        while (life >= goal && ticks < 100000) {
            life += change;
            ++ticks;
        }
        a.field_on_s = float(ticks * period);
        a.field_on_behavior = below_beh;
    }
    for (const core::XmlNode& n : cat_->resolved(a.family, a.actor))
        if (n.name == "DeathArray" && n.attr_or("index", "") == "Normal") {
            const std::string m = n.attr_or("ModelLink", "");
            if (!m.empty()) a.death_model = m;
        }
}

const std::vector<render::ActorEventRule>& EffectActorHost::unit_rules(const std::string& catalog_id) {
    auto it = unit_rules_.find(catalog_id);
    if (it != unit_rules_.end()) return it->second;
    std::vector<render::ActorEventRule> keep;
    const std::string actor = data::unit_actor_id(*cat_, catalog_id);
    std::string family = "CActorUnit";
    if (jump_look_)
        if (!cat_->find("CActorUnit", actor) && cat_->find("CActorMissile", actor)) family = "CActorMissile";
    for (render::ActorEventRule& r : rules_of(actor, family))
        if (r.head == "Behavior" || r.head == "Effect") keep.push_back(std::move(r));
    return unit_rules_.emplace(catalog_id, std::move(keep)).first->second;
}

std::string EffectActorHost::family_of(const std::string& actor) const {
    for (const char* fam : {"CActorModel", "CActorModelMaterial", "CActorSimple", "CActorBeamSimple", "CActorSound"})
        if (cat_->find(fam, actor)) return fam;
    return std::string();
}

const std::string& EffectActorHost::channel_abil(const std::string& effect) {
    static const std::string kNone;
    if (!channel_built_) {
        channel_built_ = true;
        for (const std::string& ab : cat_->ids("CAbil")) {
            const std::string eff = cat_->scalar("CAbil", ab, "Effect").value_or("");
            if (eff.empty()) continue;
            if (!cat_->actors_on("Abil", ab, "SourceChannelStart").empty() ||
                !cat_->actors_on("Abil", ab, "SourceChannelStop").empty())
                channel_abils_.emplace(eff, ab);
        }
    }
    auto it = channel_abils_.find(effect);
    return it == channel_abils_.end() ? kNone : it->second;
}

void EffectActorHost::raise_on_unit(const sim::World& w, const Edge& e, std::uint32_t unit, const char* at) {
    if (!unit_ok(w, unit)) return;
    const std::string& cid = w.units()[unit].catalog_id;
    if (cid.empty()) return;
    const std::vector<render::ActorEventRule>& rules = unit_rules(cid);
    bool hit = false;
    for (const render::ActorEventRule& r : rules)
        if (r.head == e.head && (r.subject == e.subject || r.subject.empty() || r.subject == "*") &&
            (r.sub == e.sub || r.sub.empty() || r.sub == "*")) {
            hit = true;
            break;
        }
    if (!hit) return;
    UnitLook& L = looks_[unit];
    if (L.bus.event_rules.empty()) {
        L.bus.host_owned = true;
        L.bus.route_sends = 2;
        L.bus.bind_event_rules(rules);
    }
    L.bus.raise(e.head, e.subject, e.sub, {{"At", at}});
    L.bus.pump(0.0f);
    Edge here = e;
    here.on_unit = unit;
    deliver(w, here, unit, std::string(), std::string(), std::move(L.bus.outbox));
    looks_[unit].bus.outbox.clear();
}

void EffectActorHost::deliver(const sim::World& w, const Edge& e, std::uint32_t unit, const std::string& from_actor,
                              const std::string& from_cmodel, std::vector<render::ActorEventRule> sends) {
    for (const render::ActorEventRule& r : sends) {
        if (r.verb == "ModelMaterialApply" || r.verb == "ModelMaterialApplyTo") {
            if (unit == sim::kNoTarget) continue;
            const bool named = r.verb == "ModelMaterialApply" && !r.args.empty();
            const std::string mid = named ? r.args[0] : from_cmodel;
            const formats::Model* gm = models_ && !mid.empty() ? models_(mid) : nullptr;
            if (!gm) continue;
            UnitLook& L = looks_[unit];
            bool have = false;
            for (const UnitGlaze& g : L.glazes) have = have || g.cmodel == mid;
            if (have) continue;
            UnitGlaze g;
            g.cmodel = mid;
            g.owner = from_actor;
            g.model = gm;
            L.glazes.push_back(std::move(g));
        } else if (r.verb == "ModelMaterialRemove" || r.verb == "ModelMaterialRemoveFrom") {
            auto it = looks_.find(unit);
            if (it == looks_.end()) continue;
            const std::string mid = r.verb == "ModelMaterialRemove" && !r.args.empty() ? r.args[0] : from_cmodel;
            std::vector<UnitGlaze>& gl = it->second.glazes;
            gl.erase(std::remove_if(gl.begin(), gl.end(), [&](const UnitGlaze& g) { return g.cmodel == mid; }), gl.end());
        } else if (r.verb == "Create" && !r.args.empty() && r.target.empty()) {
            const std::string fam = family_of(r.args[0]);
            if (fam == "CActorSound") {
                cue_sound_actor(r.args[0]);
            } else if (!fam.empty()) {
                bool live = false;
                for (const EffectActor& a : actors_) live = live || (a.actor == r.args[0] && a.unit == unit);
                if (!live) create(w, e, r.args[0], fam, std::string());
            } else {
                ++skipped_families_;
            }
        } else if (!r.target.empty() && r.target[0] != '_' && r.target.rfind("::", 0) != 0) {
            for (EffectActor& a : actors_)
                if (a.actor == r.target && a.unit == unit) a.inst.send(r.verb, r.args);
        } else if (r.verb.rfind("Anim", 0) == 0) {
            if (unit_send_ && unit != sim::kNoTarget) unit_send_(unit, r.verb, r.args);
        } else if (unit != sim::kNoTarget) {
            UnitLook& L = looks_[unit];
            L.bus.host_owned = true;
            L.bus.send(r.verb, r.args);
        }
    }
}

void EffectActorHost::sync_glazes() {
    for (auto& [unit, L] : looks_)
        for (UnitGlaze& g : L.glazes) {
            bool owned = false;
            for (const EffectActor& a : actors_)
                if (a.unit == unit && a.actor == g.owner && a.model == g.model) {
                    g.seq = a.seq;
                    g.time_ms = a.time_ms;
                    owned = true;
                    break;
                }
            if (owned) continue;
            if (g.seq < 0) g.seq = render::select_sequence(*g.model, {"Stand"}, unit);
            g.time_ms += 1000.0f / float(sim::World::kTicksPerSecond);
            if (g.seq >= 0 && std::size_t(g.seq) < g.model->sequences.size()) {
                const float len = float(g.model->sequences[std::size_t(g.seq)].length_ms());
                if (len > 0.0f) g.time_ms = std::fmod(g.time_ms, len);
            }
        }
}

std::size_t EffectActorHost::count(const std::string& actor) const {
    std::size_t n = 0;
    for (const EffectActor& a : actors_)
        if (a.actor == actor) ++n;
    return n;
}

const EffectActor* EffectActorHost::find(const std::string& actor) const {
    for (const EffectActor& a : actors_)
        if (a.actor == actor) return &a;
    return nullptr;
}

const std::string& EffectActorHost::name_of(std::unordered_map<std::uint32_t, std::string>& map, const char* family,
                                            std::uint32_t tag) const {
    static const std::string kNone;
    if (!cat_ || tag == 0) return kNone;
    if (map.empty())
        for (const std::string& id : cat_->ids(family)) map.emplace(behavior_id_hash(id), id);
    auto it = map.find(tag);
    return it == map.end() ? kNone : it->second;
}

std::vector<render::ActorEventRule> EffectActorHost::rules_of(const std::string& actor, const std::string& family) const {
    std::vector<render::ActorEventRule> out;
    if (!cat_) return out;
    for (const data::Catalog::ActorRule& r : cat_->actor_rules(actor, family)) {
        render::ActorEventRule er;
        er.head = r.head;
        er.subject = r.subject;
        er.sub = r.sub;
        er.filters = r.filters;
        er.verb = r.verb;
        er.args = r.args;
        er.target = r.target;
        out.push_back(std::move(er));
    }
    if (visuals_)
        if (const data::Catalog::Entry* me = cat_->find(family, actor); me && !me->parent.empty() && !cat_->find(family, me->parent))
            for (const char* fam : {"CActorModel", "CActorSimple"})
                if (cat_->find(fam, me->parent)) {
                    std::vector<render::ActorEventRule> up = rules_of(me->parent, fam);
                    up.insert(up.end(), std::make_move_iterator(out.begin()), std::make_move_iterator(out.end()));
                    return up;
                }
    return out;
}

void EffectActorHost::cue_sound_actor(const std::string& actor) {
    if (!cat_) return;
    const std::string s = field_attr(*cat_, "CActorSound", actor, "Sound", "value");
    if (s.empty()) return;
    sounds_.push_back(s);
    if (sound_) sound_(s);
}

bool EffectActorHost::resolve_model(EffectActor& a, const std::string& cmodel_override) {
    std::string mid = cmodel_override;
    if (mid.empty()) mid = cat_->scalar(a.family, a.actor, "Model").value_or("");
    if (mid.empty() || mid.find("##") != std::string::npos) mid = a.actor;
    a.cmodel = mid;
    a.model = models_ ? models_(mid) : nullptr;
    float s_actor[3], s_model[3];
    read_scale3(cat_->scalar(a.family, a.actor, "Scale").value_or(""), s_actor);
    read_scale3(cat_->scalar("CModel", mid, "ScaleMax").value_or(""), s_model);
    for (int k = 0; k < 3; ++k) {
        a.base_scale[k] = s_actor[k] * s_model[k];
        a.scale[k] = a.base_scale[k] * (a.inst.bus_scale_set ? a.inst.bus_scale : 1.0f);
    }
    a.model_evts.clear();
    for (const core::XmlNode& n : cat_->resolved("CModel", mid)) {
        if (n.name != "Events") continue;
        EffectActor::ModelEvt ev;
        for (const core::XmlNode& c : n.children) {
            if (c.name == "Anim") {
                const std::string v = c.attr_or("value", "");
                ev.anim = v.substr(0, v.find(','));
            } else if (c.name == "Name") {
                ev.name = c.attr_or("value", "");
            } else if (c.name == "Time") {
                ev.time_ms = float(std::atof(c.attr_or("value", "0").c_str()) * 1000.0);
            }
        }
        if (!ev.anim.empty() && !ev.name.empty()) a.model_evts.push_back(ev);
    }
    a.inst.model = a.model;
    for (int& c : a.inst.cached_seq) c = -2;
    return a.model != nullptr;
}

void EffectActorHost::create(const sim::World& w, const Edge& e, const std::string& actor, const std::string& family,
                             const std::string& site) {
    EffectActor a;
    a.actor = actor;
    a.family = family;
    a.head = e.head;
    a.subject = e.subject;
    a.site = site;
    a.instance = e.instance;
    a.player = e.player;
    a.site_op = field_attr(*cat_, family, actor, "HostSiteOps", "Ops");
    a.caster = e.caster;
    a.target = e.target;
    if (visuals_) {
        a.auto_scale = float(std::atof(cat_->scalar(family, actor, "AutoScaleFactor").value_or("0").c_str()));
        for (const core::XmlNode& n : cat_->resolved(family, actor))
            if (n.name == "Inherits" && n.attr_or("value", "0") == "1" &&
                (n.attr_or("index", "") == "BaseModelScale" || n.attr_or("index", "") == "Scale"))
                a.inherit_scale = true;
    }
    const bool host_subject = !field_attr(*cat_, family, actor, "Host", "Subject").empty();
    std::uint32_t unit = sim::kNoTarget;
    if (site == "Caster" || site == "Source") unit = e.caster;
    else if (site == "Target") unit = e.target;
    else if (e.on_unit != sim::kNoTarget) unit = e.on_unit;
    else if (host_subject) unit = e.target;
    if (!unit_ok(w, unit)) unit = sim::kNoTarget;
    a.unit = unit;
    if (unit != sim::kNoTarget && (site == "Caster" || site == "Source")) {
        a.x = e.src_x;
        a.y = e.src_y;
    } else if (unit != sim::kNoTarget) {
        a.x = float(w.units()[unit].x.to_double());
        a.y = float(w.units()[unit].y.to_double());
    } else {
        a.x = e.tx;
        a.y = e.ty;
    }
    a.stationary = unit == sim::kNoTarget || a.site_op.find("Stationary") != std::string::npos;
    if (visuals_ && unit != sim::kNoTarget && field_attr(*cat_, family, actor, "HostSiteOps", "HoldPosition") == "1") {
        a.stationary = true;
        a.held = true;
        if (std::size_t(unit) * 2 + 1 < last_xy_.size()) {
            a.x = last_xy_[std::size_t(unit) * 2];
            a.y = last_xy_[std::size_t(unit) * 2 + 1];
        }
    }
    a.inst.host_owned = true;
    a.inst.route_sends = visuals_ ? 1 : 0;
    a.inst.seed = std::uint64_t(created_total_ + 1);
    resolve_model(a, "");
    a.inst.bind_event_rules(rules_of(actor, family));
    a.inst.child_rules = [this](const std::string& name) {
        std::vector<render::ActorEventRule> r = rules_of(name, "CActorSound");
        return r.empty() ? rules_of(name, "CActorModel") : r;
    };
    std::vector<std::pair<std::string, std::string>> ctx;
    if (!site.empty()) ctx.emplace_back("At", site);
    ctx.emplace_back("PlayerHasReward", std::string());
    a.inst.raise("ActorCreation", std::string(), std::string(), ctx);
    a.inst.raise(e.head, e.subject, e.sub, ctx);
    a.inst.pump(0.0f);
    if (!a.inst.bus_model_swap.empty() || a.inst.bus_scale_set) resolve_model(a, a.inst.bus_model_swap);
    for (const render::ActorChild& c : a.inst.children) cue_sound_actor(c.name);
    ++created_total_;
    std::vector<render::ActorEventRule> sends = std::move(a.inst.outbox);
    a.inst.outbox.clear();
    const std::string from = a.actor, from_model = a.cmodel;
    const std::uint32_t on = a.unit;
    actors_.push_back(std::move(a));
    if (!sends.empty()) deliver(w, e, on, from, from_model, std::move(sends));
}

void EffectActorHost::edge(const sim::World& w, const Edge& e) {
    for (EffectActor& a : actors_) {
        bool mine;
        std::string at = a.site;
        if (e.head == "Effect") {
            mine = a.head == "Effect" && a.instance == e.instance;
            if (!mine && visuals_ && a.subject != e.subject && !a.from_field && a.unit != sim::kNoTarget) {
                if (a.unit == e.caster) {
                    mine = true;
                    at = "Caster";
                } else if (a.unit == e.target) {
                    mine = true;
                    at = "Target";
                }
            }
        } else {
            mine = e.on_unit != sim::kNoTarget && a.unit == e.on_unit && !a.from_field;
        }
        if (!mine) continue;
        std::vector<std::pair<std::string, std::string>> ctx;
        if (!at.empty()) ctx.emplace_back("At", at);
        a.inst.raise(e.head, e.subject, e.sub, std::move(ctx));
    }
    if (visuals_) {
        if (e.head == "Effect") {
            raise_on_unit(w, e, e.caster, "Caster");
            if (e.target != e.caster) raise_on_unit(w, e, e.target, "Target");
            if (!e.sub.empty()) {
                const std::string& abil = channel_abil(e.subject);
                if (!abil.empty()) {
                    Edge ch = e;
                    ch.head = "Abil";
                    ch.subject = abil;
                    ch.sub = e.sub == "Stop" ? "SourceChannelStop" : "SourceChannelStart";
                    ch.on_unit = e.caster;
                    edge(w, ch);
                }
            }
        } else if (e.head == "Behavior") {
            raise_on_unit(w, e, e.on_unit, "Target");
        }
    }
    for (const data::Catalog::ActorSubscription& s : cat_->actors_on(e.head, e.subject, e.sub)) {
        if (s.family.empty()) continue;
        std::string site;
        bool other = false;
        for (const auto& f : s.filters) {
            if (f.first == "At") site = f.second;
            else other = true;
        }
        if (other) {
            ++skipped_filters_;
            continue;
        }
        if (site == "Target" && !unit_ok(w, e.target) && e.head == "Behavior") continue;
        if (s.family == "CActorSound") {
            cue_sound_actor(s.actor);
            continue;
        }
        if (s.family == "CActorDoodad") continue;
        const bool drawn_here = visuals_ && (s.family == "CActorModelMaterial" || s.family == "CActorSimple" || s.family == "CActorBeamSimple");
        if (s.family != "CActorModel" && !drawn_here) {
            ++skipped_families_;
            continue;
        }
        create(w, e, s.actor, s.family, site);
    }
}

void EffectActorHost::create_field_actor(const sim::World& w, const sim::ForceFieldInstance& ff, std::uint64_t key) {
    const std::string& unit = name_of(unit_names_, "CUnit", ff.unit_tag);
    if (unit.empty()) return;
    auto it = unit_actor_.find(unit);
    if (it == unit_actor_.end()) {
        std::string found;
        for (const std::string& id : cat_->ids("CActorUnit"))
            if (cat_->actor_unit_name(id, "CActorUnit") == unit) {
                found = id;
                break;
            }
        it = unit_actor_.emplace(unit, found).first;
    }
    if (it->second.empty()) return;
    for (const sim::Unit& u : w.units())
        if (u.alive && u.catalog_id == unit && u.x == ff.x && u.y == ff.y) return;
    EffectActor a;
    a.actor = it->second;
    a.family = "CActorUnit";
    a.head = "UnitBirth";
    a.subject = unit;
    a.from_field = true;
    a.field_key = key;
    a.stationary = true;
    a.x = float(ff.x.to_double());
    a.y = float(ff.y.to_double());
    a.inst.host_owned = true;
    a.inst.seed = std::uint64_t(created_total_ + 1);
    resolve_model(a, "");
    a.inst.bind_event_rules(rules_of(a.actor, "CActorUnit"));
    a.inst.raise("ActorCreation");
    a.inst.pump(0.0f);
    if (field_life_) field_life_rules(a, unit);
    ++created_total_;
    actors_.push_back(std::move(a));
}

void EffectActorHost::model_events(EffectActor& a, int seq_before, float t_before) {
    if (a.model_evts.empty() || !a.model) return;
    const int seq = a.inst.current_sequence();
    if (seq < 0 || std::size_t(seq) >= a.model->sequences.size()) return;
    const float t = a.inst.current_time();
    const std::string& nm = a.model->sequences[std::size_t(seq)].name;
    const std::string tok = nm.substr(0, nm.find(' '));
    for (const EffectActor::ModelEvt& ev : a.model_evts) {
        if (ev.anim != tok) continue;
        bool crossed;
        if (seq != seq_before) crossed = ev.time_ms <= t;
        else if (t >= t_before) crossed = t_before < ev.time_ms && ev.time_ms <= t;
        else crossed = ev.time_ms > t_before || ev.time_ms <= t;
        if (crossed) a.inst.raise("ModelEvent", std::string(), std::string(), {{"ModelEventName", ev.name}});
    }
}

void EffectActorHost::observe(const sim::World& w, float dt_ms, bool ticked) {
    sounds_.clear();
    if (!cat_) return;
    if (!w.presentation_log()) {
        actors_.clear();
        return;
    }
    for (const sim::PresentationEvent& pe : w.presentation_events()) {
        if (pe.seq <= last_seq_) continue;
        last_seq_ = pe.seq;
        if (pe.abil) {
            const std::string& aid = name_of(abil_names_, "CAbil", pe.fx_tag);
            if (aid.empty()) continue;
            Edge e;
            e.head = "Abil";
            e.subject = aid;
            e.sub = "SourceCastStart";
            e.instance = pe.instance;
            e.caster = pe.source;
            e.target = pe.target;
            e.on_unit = pe.source;
            e.src_x = float(pe.src_x.to_double());
            e.src_y = float(pe.src_y.to_double());
            e.tx = float(pe.tx.to_double());
            e.ty = float(pe.ty.to_double());
            e.player = pe.player;
            edge(w, e);
            continue;
        }
        const std::string& id = name_of(effect_names_, "CEffect", pe.fx_tag);
        if (id.empty()) continue;
        Edge e;
        e.head = "Effect";
        e.subject = id;
        e.sub = pe.stop ? "Stop" : "Start";
        e.instance = pe.instance;
        e.caster = pe.source;
        e.target = pe.target;
        e.src_x = float(pe.src_x.to_double());
        e.src_y = float(pe.src_y.to_double());
        e.tx = float(pe.tx.to_double());
        e.ty = float(pe.ty.to_double());
        e.player = pe.player;
        edge(w, e);
    }
    if (ticked) {
        std::map<std::uint32_t, std::set<std::uint32_t>> now;
        for (const sim::Unit& u : w.units()) {
            if (!u.alive || u.behaviors.empty()) continue;
            for (const sim::BehaviorInstance& b : u.behaviors) {
                const std::uint32_t tag = b.src != 0 ? b.src : b.present_src;
                if (tag != 0) now[u.id].insert(tag);
            }
        }
        auto beh_edge = [&](std::uint32_t unit, std::uint32_t tag, const char* sub, std::uint32_t caster) {
            const std::string& id = name_of(behavior_names_, "CBehavior", tag);
            if (id.empty()) return;
            Edge e;
            e.head = "Behavior";
            e.subject = id;
            e.sub = sub;
            e.on_unit = unit;
            e.caster = caster;
            e.target = unit;
            if (unit < w.units().size()) {
                e.src_x = e.tx = float(w.units()[unit].x.to_double());
                e.src_y = e.ty = float(w.units()[unit].y.to_double());
                e.player = w.units()[unit].player;
            }
            edge(w, e);
        };
        for (const auto& [unit, tags] : held_) {
            auto nit = now.find(unit);
            for (std::uint32_t t : tags)
                if (nit == now.end() || !nit->second.count(t)) beh_edge(unit, t, "Off", sim::kNoTarget);
        }
        for (const auto& [unit, tags] : now) {
            auto pit = held_.find(unit);
            for (std::uint32_t t : tags)
                if (pit == held_.end() || !pit->second.count(t)) {
                    std::uint32_t caster = sim::kNoTarget;
                    for (const sim::BehaviorInstance& b : w.units()[unit].behaviors)
                        if ((b.src != 0 ? b.src : b.present_src) == t) { caster = b.caster; break; }
                    beh_edge(unit, t, "On", caster);
                }
        }
        held_ = std::move(now);
        std::map<std::uint64_t, bool> seen;
        for (const sim::ForceFieldInstance& ff : w.force_fields()) {
            if (ff.unit_tag == 0) continue;
            const std::uint64_t k = field_key_of(ff);
            seen[k] = true;
            if (!fields_.count(k)) create_field_actor(w, ff, k);
            if (!field_life_) continue;
            const float elapsed = float((ff.duration - ff.timer).to_double());
            for (EffectActor& a : actors_)
                if (a.from_field && !a.dying && a.field_key == k && a.field_on_s >= 0.0f && elapsed >= a.field_on_s) {
                    a.field_on_s = -1.0f;
                    a.inst.raise("Behavior", a.field_on_behavior, "On");
                    a.inst.pump(0.0f);
                    if (!a.inst.bus_model_swap.empty() && a.inst.bus_model_swap != a.cmodel)
                        resolve_model(a, a.inst.bus_model_swap);
                    for (const render::ActorChild& c : a.inst.children) cue_sound_actor(c.name);
                }
            auto bit = body_life_.find(k);
            if (bit == body_life_.end()) {
                const std::string& unit = name_of(unit_names_, "CUnit", ff.unit_tag);
                BodyLife bl;
                if (!unit.empty())
                    for (const sim::Unit& u : w.units())
                        if (u.alive && u.catalog_id == unit && u.x == ff.x && u.y == ff.y) {
                            bl.unit = u.id;
                            break;
                        }
                if (bl.unit == sim::kNoTarget) continue;
                EffectActor probe;
                probe.actor = data::unit_actor_id(*cat_, unit);
                probe.family = "CActorUnit";
                field_life_rules(probe, unit);
                bl.on_s = probe.field_on_s;
                bl.behavior = probe.field_on_behavior;
                bit = body_life_.emplace(k, std::move(bl)).first;
            }
            BodyLife& bl = bit->second;
            if (bl.on_s >= 0.0f && elapsed >= bl.on_s && unit_ok(w, bl.unit)) {
                bl.on_s = -1.0f;
                const sim::Unit& u = w.units()[bl.unit];
                Edge e;
                e.head = "Behavior";
                e.subject = bl.behavior;
                e.sub = "On";
                e.on_unit = e.caster = e.target = bl.unit;
                e.src_x = e.tx = float(u.x.to_double());
                e.src_y = e.ty = float(u.y.to_double());
                e.player = u.player;
                edge(w, e);
            }
        }
        for (auto bit = body_life_.begin(); bit != body_life_.end();)
            bit = seen.count(bit->first) ? std::next(bit) : body_life_.erase(bit);
        for (EffectActor& a : actors_)
            if (a.from_field && !a.dying && !seen.count(a.field_key)) {
                a.dying = true;
                if (field_life_ && !a.death_model.empty() && a.death_model != a.cmodel) resolve_model(a, a.death_model);
                a.inst.trigger(render::AnimState::Death);
            }
        fields_ = std::move(seen);
    }
    for (std::size_t i = 0; i < actors_.size();) {
        EffectActor& a = actors_[i];
        bool gone = false;
        if (a.unit != sim::kNoTarget) {
            if (!unit_ok(w, a.unit)) {
                if (a.orphan_ms < 0.0f) {
                    a.orphan_ms = 0.0f;
                    a.inst.raise("ActorOrphan");
                }
            } else if (!a.stationary) {
                a.x = float(w.units()[a.unit].x.to_double());
                a.y = float(w.units()[a.unit].y.to_double());
            }
        }
        if (!gone) {
            const int s0 = a.inst.current_sequence();
            const float t0 = a.inst.current_time();
            a.inst.advance(dt_ms);
            model_events(a, s0, t0);
            std::vector<std::string> before;
            for (const render::ActorChild& c : a.inst.children) before.push_back(c.name);
            a.inst.pump(dt_ms);
            for (const render::ActorChild& c : a.inst.children)
                if (std::find(before.begin(), before.end(), c.name) == before.end()) cue_sound_actor(c.name);
            if (a.inst.bus_scale_set)
                for (int k = 0; k < 3; ++k) a.scale[k] = a.base_scale[k] * a.inst.bus_scale;
            if (!a.inst.outbox.empty()) {
                std::vector<render::ActorEventRule> sends = std::move(a.inst.outbox);
                a.inst.outbox.clear();
                const std::string from = a.actor, from_model = a.cmodel;
                Edge none;
                deliver(w, none, a.unit, from, from_model, std::move(sends));
            }
            a.age_ms += dt_ms;
            if (a.inst.self_destroyed) gone = true;
            if (a.orphan_ms >= 0.0f && (a.orphan_ms += dt_ms) > kOrphanMs) gone = true;
            if (a.dying) {
                const int ds = a.inst.sequence_of(render::AnimState::Death);
                const float len = (a.model && ds >= 0) ? float(a.model->sequences[std::size_t(ds)].length_ms()) : 0.0f;
                if (len <= 0.0f || a.inst.clock >= len) gone = true;
            }
        }
        if (gone) {
            ++destroyed_total_;
            if (auto lit = looks_.find(a.unit); lit != looks_.end()) {
                std::vector<UnitGlaze>& gl = lit->second.glazes;
                const std::string& owner = a.actor;
                gl.erase(std::remove_if(gl.begin(), gl.end(), [&](const UnitGlaze& g) { return g.owner == owner; }), gl.end());
            }
            actors_.erase(actors_.begin() + long(i));
        } else {
            a.seq = a.inst.current_sequence();
            a.time_ms = a.inst.current_time();
            ++i;
        }
    }
    if (visuals_ && ticked && jump_look_) watch_jumps(w);
    if (visuals_ && ticked) {
        last_xy_.resize(w.units().size() * 2);
        for (const sim::Unit& u : w.units()) {
            last_xy_[std::size_t(u.id) * 2] = float(u.x.to_double());
            last_xy_[std::size_t(u.id) * 2 + 1] = float(u.y.to_double());
        }
    }
    if (!looks_.empty()) {
        for (auto it = looks_.begin(); it != looks_.end();) {
            if (!unit_ok(w, it->first)) {
                it = looks_.erase(it);
                continue;
            }
            it->second.bus.pump(dt_ms);
            ++it;
        }
        sync_glazes();
    }
}

}
