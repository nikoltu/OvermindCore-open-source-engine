#include "sc2/data/actor_catalog.hpp"

#include "sc2/core/xml.hpp"
#include "sc2/data/catalog.hpp"

namespace sc2::data {

using core::XmlNode;

namespace {

bool ident_char(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_';
}

std::string expand_consts(const Catalog& cat, const std::string& in) {
    std::string out;
    out.reserve(in.size());
    for (size_t i = 0; i < in.size();) {
        if (in[i] == '$') {
            size_t j = i + 1;
            while (j < in.size() && ident_char(in[j])) ++j;
            if (j > i + 1) {
                if (const std::string* v = cat.const_value(in.substr(i, j - i))) {
                    out += *v;
                    i = j;
                    continue;
                }
            }
        }
        out.push_back(in[i++]);
    }
    return out;
}

void replace_all(std::string& s, const std::string& from, const std::string& to) {
    if (from.empty()) return;
    for (size_t p; (p = s.find(from)) != std::string::npos;)
        s.replace(p, from.size(), to);
}

std::string expand(const Catalog& cat, std::string s, const std::string& unit_name,
                   const std::string& self_id) {
    s = expand_consts(cat, s);
    if (!unit_name.empty()) replace_all(s, "##unitName##", unit_name);
    if (!self_id.empty()) replace_all(s, "##id##", self_id);
    return s;
}

std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return {};
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

std::vector<std::string> split(const std::string& s, char sep) {
    std::vector<std::string> out;
    size_t start = 0;
    for (size_t i = 0;; ++i) {
        if (i == s.size() || s[i] == sep) {
            std::string tok = trim(s.substr(start, i - start));
            if (!tok.empty()) out.push_back(tok);
            if (i == s.size()) break;
            start = i + 1;
        }
    }
    return out;
}

ActorRule make_rule(std::string terms, std::string target, std::string send) {
    ActorRule r;
    r.terms = terms;
    r.target = std::move(target);
    r.send = send;

    size_t sc = terms.find(';');
    r.event = trim(sc == std::string::npos ? terms : terms.substr(0, sc));
    r.event_head = r.event.substr(0, r.event.find('.'));
    if (sc != std::string::npos)
        for (auto& f : split(terms.substr(sc + 1), ';')) r.filters.push_back(f);

    std::vector<std::string> stoks = split(send, ' ');
    if (!stoks.empty()) {
        r.verb = stoks.front();
        r.args.assign(stoks.begin() + 1, stoks.end());
    }
    return r;
}

std::optional<std::string> prop(const XmlNode& node, const std::string& name) {
    if (const std::string* a = node.attr(name)) return *a;
    for (const XmlNode& c : node.children) {
        if (c.name != name) continue;
        if (const std::string* v = c.attr("value")) return *v;
        if (const std::string* l = c.attr("Link")) return *l;
        if (!c.text.empty()) return c.text;
        return std::string{};
    }
    return std::nullopt;
}

}

void ActorCatalog::build(const Catalog& cat) {
    actor_units_.clear();
    by_unit_name_.clear();
    macros_.clear();
    models_.clear();

    for (const std::string& id : cat.ids("CActorEventMacro")) {
        CActorEventMacro m;
        m.id = id;
        if (const Catalog::Entry* e = cat.find("CActorEventMacro", id)) m.parent = e->parent;
        for (const XmlNode& c : cat.resolved("CActorEventMacro", id)) {
            if (c.name != "On") continue;
            m.on.push_back(make_rule(expand(cat, c.attr_or("Terms", ""), "", ""),
                                     c.attr_or("Target", ""),
                                     expand(cat, c.attr_or("Send", ""), "", "")));
        }
        macros_[id] = std::move(m);
    }

    for (const std::string& id : cat.ids("CModel")) {
        CModel m;
        m.id = id;
        if (const Catalog::Entry* e = cat.find("CModel", id)) m.parent = e->parent;
        if (auto mv = cat.scalar("CModel", id, "Model")) m.model = *mv;
        for (const XmlNode& c : cat.resolved("CModel", id)) {
            if (c.name != "Events") continue;
            CModel::Event ev;
            ev.name = prop(c, "Name").value_or("");
            ev.anim = prop(c, "Anim").value_or("");
            ev.time = prop(c, "Time").value_or("");
            m.events.push_back(std::move(ev));
        }
        models_[id] = std::move(m);
    }

    for (const std::string& id : cat.ids("CActorUnit")) {
        CActorUnit a;
        a.id = id;
        const Catalog::Entry* e = cat.find("CActorUnit", id);
        if (e) {
            a.parent = e->parent;
            if (const std::string* un = e->attr("unitName")) a.unit_name = *un;
        }
        if (a.unit_name.empty()) a.unit_name = id;

        if (auto mv = cat.scalar("CActorUnit", id, "Model"))
            a.model = expand(cat, *mv, a.unit_name, id);
        a.walk_anim_move_speed = cat.scalar_num("CActorUnit", id, "WalkAnimMoveSpeed");

        const std::vector<XmlNode>& fields = cat.resolved("CActorUnit", id);

        for (const XmlNode& c : fields) {
            if (c.name != "Baselines") continue;
            std::string idx = c.attr_or("index", "");
            std::string ap = prop(c, "AnimProps").value_or("");
            bool found = false;
            for (auto& b : a.baselines)
                if (b.first == idx) { b.second = ap; found = true; break; }
            if (!found) a.baselines.emplace_back(idx, ap);
        }

        for (const XmlNode& c : fields) {
            if (c.name != "DeathArray") continue;
            std::string idx = c.attr_or("index", "");
            ActorDeath* row = nullptr;
            for (auto& d : a.death_array)
                if (d.index == idx) { row = &d; break; }
            if (!row) {
                a.death_array.push_back(ActorDeath{idx, "", "", ""});
                row = &a.death_array.back();
            }
            if (auto v = prop(c, "AnimProps"))
                row->anim_props = expand(cat, *v, a.unit_name, id);
            if (auto v = prop(c, "ModelLink"))
                row->model_link = expand(cat, *v, a.unit_name, id);
            if (auto v = prop(c, "SoundLink"))
                row->sound_link = expand(cat, *v, a.unit_name, id);
        }

        for (const XmlNode& c : fields) {
            if (c.name == "On") {
                a.on.push_back(
                    make_rule(expand(cat, c.attr_or("Terms", ""), a.unit_name, id),
                              c.attr_or("Target", ""),
                              expand(cat, c.attr_or("Send", ""), a.unit_name, id)));
            } else if (c.name == "Macros") {
                std::string mid = c.attr_or("value", "");
                if (mid.empty()) continue;
                a.macros.push_back(mid);
                auto mit = macros_.find(mid);
                if (mit == macros_.end()) continue;
                for (const XmlNode& mc : cat.resolved("CActorEventMacro", mid)) {
                    if (mc.name != "On") continue;
                    a.on.push_back(make_rule(
                        expand(cat, mc.attr_or("Terms", ""), a.unit_name, id),
                        mc.attr_or("Target", ""),
                        expand(cat, mc.attr_or("Send", ""), a.unit_name, id)));
                }
            }
        }

        by_unit_name_.emplace(a.unit_name, id);
        actor_units_[id] = std::move(a);
    }
}

const CActorUnit* ActorCatalog::actor_unit(const std::string& id) const {
    auto it = actor_units_.find(id);
    return it == actor_units_.end() ? nullptr : &it->second;
}

const CActorUnit* ActorCatalog::actor_for_unit(const std::string& unit_name) const {
    auto it = by_unit_name_.find(unit_name);
    if (it != by_unit_name_.end()) return actor_unit(it->second);
    return actor_unit(unit_name);
}

const CActorEventMacro* ActorCatalog::event_macro(const std::string& id) const {
    auto it = macros_.find(id);
    return it == macros_.end() ? nullptr : &it->second;
}

const CModel* ActorCatalog::model(const std::string& id) const {
    auto it = models_.find(id);
    return it == models_.end() ? nullptr : &it->second;
}

std::vector<std::string> ActorCatalog::actor_unit_ids() const {
    std::vector<std::string> out;
    out.reserve(actor_units_.size());
    for (const auto& [id, a] : actor_units_) {
        (void)a;
        out.push_back(id);
    }
    return out;
}

}
