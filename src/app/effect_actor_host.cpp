#include "omc/app/effect_actor_host.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

#include "omc/app/ability.hpp"
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
    a.inst.host_owned = true;
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
    actors_.push_back(std::move(a));
}

void EffectActorHost::edge(const sim::World& w, const Edge& e) {
    for (EffectActor& a : actors_) {
        bool mine;
        if (e.head == "Effect")
            mine = a.head == "Effect" && a.instance == e.instance;
        else
            mine = e.on_unit != sim::kNoTarget && a.unit == e.on_unit && !a.from_field;
        if (!mine) continue;
        std::vector<std::pair<std::string, std::string>> ctx;
        if (!a.site.empty()) ctx.emplace_back("At", a.site);
        a.inst.raise(e.head, e.subject, e.sub, std::move(ctx));
    }
    for (const data::Catalog::ActorSubscription& s : cat_->actors_on(e.head, e.subject, e.sub)) {
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
        if (s.family != "CActorModel") {
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
        }
        for (EffectActor& a : actors_)
            if (a.from_field && !a.dying && !seen.count(a.field_key)) {
                a.dying = true;
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
            actors_.erase(actors_.begin() + long(i));
        } else {
            a.seq = a.inst.current_sequence();
            a.time_ms = a.inst.current_time();
            ++i;
        }
    }
}

}
