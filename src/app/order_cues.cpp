#include "omc/app/order_cues.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include <cmath>
#include <memory>

#include "omc/app/hud_render.hpp"
#include "omc/app/melee_game.hpp"
#include "omc/app/melee_models.hpp"
#include "omc/data/actor_model.hpp"
#include "omc/data/catalog.hpp"
#include "omc/render/anim.hpp"
#include "omc/render/mat4.hpp"
#include "omc/render/prim.hpp"

namespace omc::app {

namespace {

std::string trim(const std::string& s) {
    std::size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t')) ++a;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t')) --b;
    return s.substr(a, b - a);
}

std::vector<std::string> split_terms(const std::string& terms) {
    std::vector<std::string> out;
    std::size_t i = 0;
    while (i <= terms.size()) {
        const std::size_t semi = terms.find(';', i);
        const std::string part = trim(terms.substr(i, semi == std::string::npos ? std::string::npos : semi - i));
        if (!part.empty()) out.push_back(part);
        if (semi == std::string::npos) break;
        i = semi + 1;
    }
    return out;
}

bool has_term(const std::vector<std::string>& terms, const std::string& t) {
    return std::find(terms.begin(), terms.end(), t) != terms.end();
}

bool parse_rgb(const std::string& v, std::array<float, 3>& out) {
    int r = 0, g = 0, b = 0;
    if (std::sscanf(v.c_str(), "%d,%d,%d", &r, &g, &b) != 3) return false;
    out = {float(r) / 255.0f, float(g) / 255.0f, float(b) / 255.0f};
    return true;
}

bool parse_argb(const std::string& v, std::array<std::uint8_t, 4>& out) {
    int a = 255, r = 0, g = 0, b = 0;
    if (std::sscanf(v.c_str(), "%d,%d,%d,%d", &a, &r, &g, &b) != 4) return false;
    out = {std::uint8_t(a), std::uint8_t(r), std::uint8_t(g), std::uint8_t(b)};
    return true;
}

std::string field_of(const core::XmlNode& n, const std::string& name) {
    if (const std::string* a = n.attr(name)) return *a;
    for (const core::XmlNode& c : n.children)
        if (c.name == name) return c.attr_or("value", std::string());
    return {};
}

float first_float(const std::string& v, float fallback) {
    if (v.empty()) return fallback;
    char* end = nullptr;
    const double d = std::strtod(v.c_str(), &end);
    return end == v.c_str() ? fallback : float(d);
}

}

std::vector<ConfirmActorSpec> confirmation_actor_specs(const data::Catalog& cat) {
    std::vector<ConfirmActorSpec> out;
    for (const std::string& id : cat.ids("CActorModel")) {
        const data::Catalog::Entry* e = cat.find("CActorModel", id);
        if (!e || e->is_default) continue;
        ConfirmActorSpec s;
        bool create = false;
        for (const core::XmlNode& n : cat.resolved("CActorModel", id)) {
            if (n.name != "On") continue;
            const std::vector<std::string> terms = split_terms(n.attr_or("Terms", std::string()));
            const std::string send = n.attr_or("Send", std::string());
            if (terms.empty()) continue;
            if (send == "Create" && terms.front().rfind("Confirmation.", 0) == 0 &&
                terms.front().size() > 6 && terms.front().compare(terms.front().size() - 6, 6, ".Start") == 0) {
                if (!has_term(terms, "Game Local")) continue;
                create = true;
                s.attack = has_term(terms, "AbilCmd attack");
                if (has_term(terms, "ValidateUnit IsFlying")) s.air = 1;
                else if (has_term(terms, "ValidateUnit IsNotFlying")) s.air = 0;
            } else if (terms.front() == "ActorCreation" && send.rfind("SetTintColor ", 0) == 0) {
                s.tinted = parse_rgb(trim(send.substr(13)), s.tint);
            }
        }
        if (!create) continue;
        s.actor = id;
        s.model_id = cat.scalar("CActorModel", id, "Model").value_or(id);
        if (s.model_id.empty()) s.model_id = id;
        out.push_back(std::move(s));
    }
    return out;
}

const ConfirmActorSpec* pick_confirmation(const std::vector<ConfirmActorSpec>& specs, bool attack, bool air) {
    const ConfirmActorSpec* any = nullptr;
    for (const ConfirmActorSpec& s : specs) {
        if (s.attack != attack) continue;
        if (s.air == (air ? 1 : 0)) return &s;
        if (s.air < 0 && !any) any = &s;
    }
    return any;
}

FlashLook selection_flash_look(const data::Catalog& cat) {
    FlashLook f;
    for (const std::string& id : cat.ids("CActorSelection")) {
        const data::Catalog::Entry* e = cat.find("CActorSelection", id);
        if (!e || e->is_default) continue;
        bool local = false;
        for (const core::XmlNode& n : cat.resolved("CActorSelection", id)) {
            if (n.name != "On" || n.attr_or("Send", std::string()) != "Create") continue;
            const std::vector<std::string> terms = split_terms(n.attr_or("Terms", std::string()));
            if (!terms.empty() && terms.front() == "SelectionLocalFlash" && !has_term(terms, "ValidateUnit UseCliffSelection"))
                local = true;
        }
        if (!local) continue;
        f.found = true;
        f.actor = id;
        auto num = [&](const char* field, float fallback) {
            return first_float(cat.scalar("CActorSelection", id, field).value_or(std::string()), fallback);
        };
        f.alpha = num("Alpha", f.alpha);
        f.inner_width = num("InnerWidth", f.inner_width);
        f.outer_width = num("OuterWidth", f.outer_width);
        f.rotation = num("RotationSpeed", f.rotation);
        f.segments = std::max(1, int(num("SegmentCount", float(f.segments)) + 0.5f));
        f.solid = num("SegmentPercentSolid", f.solid);
        f.hold_s = num("HoldTime", f.hold_s);
        break;
    }
    return f;
}

double effect_area_radius(const data::Catalog& cat, const std::string& effect) {
    std::vector<std::string> stack{effect}, seen;
    double reveal = 0.0;
    while (!stack.empty() && seen.size() < 64) {
        const std::string cur = stack.back();
        stack.pop_back();
        if (cur.empty() || std::find(seen.begin(), seen.end(), cur) != seen.end()) continue;
        seen.push_back(cur);
        const data::Catalog::Entry* e = cat.find("CEffect", cur);
        if (!e) continue;
        double area = 0.0;
        std::vector<std::string> next;
        for (const core::XmlNode& n : cat.resolved("CEffect", cur)) {
            if (n.name == "AreaArray") {
                if (const std::string* r = n.attr("Radius")) area = std::max(area, std::strtod(r->c_str(), nullptr));
                if (const std::string* x = n.attr("Effect"); x && !x->empty()) next.push_back(*x);
            } else if (n.name == "EffectArray" || n.name == "InitialEffect" || n.name == "PeriodicEffectArray") {
                const std::string v = n.attr_or("value", std::string());
                if (!v.empty()) next.push_back(v);
            } else if (n.name == "RevealRadius" && reveal <= 0.0) {
                reveal = std::strtod(n.attr_or("value", std::string("0")).c_str(), nullptr);
            }
        }
        if (area > 0.0) return area;
        for (auto it = next.rbegin(); it != next.rend(); ++it) stack.push_back(*it);
    }
    return reveal;
}

std::unordered_map<std::string, std::string> cursor_splat_actors(const data::Catalog& cat) {
    std::unordered_map<std::string, std::string> out;
    for (const std::string& id : cat.ids("CActorSplat")) {
        const data::Catalog::Entry* e = cat.find("CActorSplat", id);
        if (!e || e->is_default) continue;
        std::vector<std::string> removed;
        const core::XmlNodeList& rows = cat.resolved("CActorSplat", id);
        for (const core::XmlNode& n : rows)
            if (n.name == "Remove") removed.push_back(cat.expand_entry_tokens("CActorSplat", id, n.attr_or("Terms", std::string())));
        for (const core::XmlNode& n : rows) {
            if (n.name != "On" || n.attr_or("Send", std::string()) != "Create") continue;
            const std::string raw = n.attr_or("Terms", std::string());
            const std::vector<std::string> terms = split_terms(cat.expand_entry_tokens("CActorSplat", id, raw));
            if (terms.empty()) continue;
            const std::string& t = terms.front();
            if (t.rfind("Abil.", 0) != 0 || t.size() <= 12 || t.compare(t.size() - 7, 7, ".Cursor") != 0) continue;
            if (t.find("##") != std::string::npos) continue;
            if (std::find(removed.begin(), removed.end(), t) != removed.end()) continue;
            const std::string abil = t.substr(5, t.size() - 12);
            if (!abil.empty() && abil != "*") out[abil] = id;
        }
    }
    return out;
}

CursorSplatSpec ability_cursor_splat(const data::Catalog& cat, const std::string& abil) {
    return ability_cursor_splat(cat, cursor_splat_actors(cat), abil);
}

CursorSplatSpec ability_cursor_splat(const data::Catalog& cat, const std::unordered_map<std::string, std::string>& actors,
                                     const std::string& abil) {
    CursorSplatSpec s;
    std::string eff = cat.scalar("CAbil", abil, "CursorEffect").value_or(std::string());
    if (eff.empty()) return s;
    eff = cat.expand_entry_tokens("CAbil", abil, eff);
    s.effect = eff;
    s.radius = effect_area_radius(cat, eff);
    const auto it = actors.find(abil);
    if (it != actors.end()) {
        s.actor = it->second;
        s.model_id = cat.scalar("CActorSplat", s.actor, "Model").value_or(s.actor);
        if (s.model_id.empty()) s.model_id = s.actor;
    }
    s.found = s.radius > 0.0 && !s.actor.empty();
    return s;
}

OrderDisplay ability_order_display(const data::Catalog& cat, const std::string& unit_id, const std::string& abil_tag,
                                   int index) {
    OrderDisplay d;
    auto fill = [&](const core::XmlNode& n) {
        const std::string dt = field_of(n, "DisplayType");
        const std::string col = field_of(n, "Color");
        const std::string mod = field_of(n, "Model");
        const std::string tex = field_of(n, "LineTexture");
        if (!dt.empty() && d.display_type.empty()) d.display_type = dt;
        if (!col.empty() && !d.found) d.found = parse_argb(col, d.argb);
        if (!mod.empty() && d.model_path.empty()) d.model_path = mod;
        if (!tex.empty() && d.line_texture.empty()) d.line_texture = tex;
    };
    auto from_rows = [&](const std::vector<const core::XmlNode*>& indexed, const std::vector<const core::XmlNode*>& plain) {
        int max_idx = -1;
        for (const core::XmlNode* n : indexed) {
            const int i = std::atoi(n->attr_or("index", std::string("0")).c_str());
            max_idx = std::max(max_idx, i);
            if (i == index) fill(*n);
        }
        for (std::size_t k = 0; k < plain.size(); ++k)
            if (int(k) + max_idx + 1 == index) fill(*plain[k]);
    };
    std::string tag = abil_tag;
    if (!unit_id.empty())
        for (const std::string& ab : cat.array_links("CUnit", unit_id, "AbilArray")) {
            const data::Catalog::Entry* e = ab.empty() ? nullptr : cat.find("CAbil", ab);
            if (!e || e->tag != abil_tag) continue;
            d.ability = ab;
            std::vector<const core::XmlNode*> idx, plain;
            for (const core::XmlNode& n : cat.resolved("CAbil", ab))
                if (n.name == "OrderArray") (n.attr("index") ? idx : plain).push_back(&n);
            from_rows(idx, plain);
            break;
        }
    for (const std::string& t : {tag, std::string("CAbil")}) {
        std::vector<const core::XmlNode*> idx, plain;
        if (const std::vector<core::XmlNode>* v = cat.tag_default_indexed_nodes(t))
            for (const core::XmlNode& n : *v)
                if (n.name == "OrderArray") idx.push_back(&n);
        if (const std::vector<core::XmlNode>* v = cat.tag_default_nodes(t))
            for (const core::XmlNode& n : *v)
                if (n.name == "OrderArray") plain.push_back(&n);
        if (t == "CAbil" && idx.empty() && !plain.empty()) {
            fill(*plain.front());
            continue;
        }
        from_rows(idx, plain);
    }
    return d;
}

std::string command_harness_model(const data::Catalog& cat, const std::string& display_type, const std::string& race) {
    if (display_type.empty() || race.empty()) return {};
    const std::string want = "OrderDisplayType " + display_type;
    for (const std::string& id : cat.ids("CActorModel")) {
        const data::Catalog::Entry* e = cat.find("CActorModel", id);
        if (!e || e->is_default) continue;
        for (const core::XmlNode& n : cat.resolved("CActorModel", id)) {
            if (n.name != "On" || n.attr_or("Send", std::string()) != "Create") continue;
            const std::vector<std::string> terms = split_terms(n.attr_or("Terms", std::string()));
            if (terms.empty() || terms.front() != "CommandUI.*.Birth" || !has_term(terms, want)) continue;
            bool race_ok = true;
            for (const std::string& t : terms) {
                if (t.rfind("ValidatePlayer ", 0) != 0) continue;
                const std::string v = trim(t.substr(15));
                race_ok = cat.scalar("CValidator", v, "Value").value_or(std::string()) == race;
            }
            if (!race_ok) continue;
            const std::string m = cat.scalar("CActorModel", id, "Model").value_or(id);
            return m.empty() ? id : m;
        }
    }
    return {};
}

PathLook waypoint_path_look(const data::Catalog& cat) {
    PathLook p;
    for (const char* entry : {"Dflt", "CoreDflt"}) {
        bool any = false;
        for (const core::XmlNode& n : cat.resolved("CGameUI", entry)) {
            const std::string v = n.attr_or("value", std::string());
            const std::string idx = n.attr_or("index", std::string());
            if (v.empty()) continue;
            if (n.name == "WayPointLineWidth") { p.width = first_float(v, p.width); any = true; }
            else if (n.name == "WayPointTileLength") { p.tile = first_float(v, p.tile); any = true; }
            else if (n.name == "WayPointMultiUnitFadePoint") { p.fade_point = first_float(v, p.fade_point); any = true; }
            else if (n.name == "WayPointMultiUnitFadeAlpha") { p.fade_alpha = first_float(v, p.fade_alpha); any = true; }
            else if (n.name == "DefaultPathTexture" && idx == "World") {
                p.texture = v;
                for (char& c : p.texture)
                    if (c == '\\') c = '/';
                any = true;
            } else if (n.name == "DefaultPathColor" && idx == "World") {
                int r = 255, g = 255, b = 255, a = 255;
                if (std::sscanf(v.c_str(), "%d,%d,%d,%d", &r, &g, &b, &a) == 4) p.argb = {std::uint8_t(a), std::uint8_t(r), std::uint8_t(g), std::uint8_t(b)};
                any = true;
            } else if (n.name == "DefaultPathStepModel" && idx == "World") {
                p.step_model = v;
                for (char& c : p.step_model)
                    if (c == '\\') c = '/';
                any = true;
            } else if (n.name == "DefaultPathStepModelScale" && idx == "World") {
                p.step_scale = first_float(v, p.step_scale);
                any = true;
            }
        }
        if (any) break;
    }
    if (p.width <= 0.0f) p.width = 0.15f;
    if (p.tile <= 0.0f) p.tile = 0.8f;
    return p;
}

std::string placeholder_actor(const data::Catalog& cat, const std::string& unit_id) {
    if (unit_id.empty()) return {};
    std::string actor = data::unit_actor_id(cat, unit_id);
    if (actor.empty()) actor = unit_id;
    if (auto v = cat.scalar("CActorUnit", actor, "PlaceholderActorModel"); v && !v->empty()) return *v;
    if (const std::vector<core::XmlNode>* d = cat.tag_default_nodes("CActorUnit"))
        for (const core::XmlNode& n : *d)
            if (n.name == "PlaceholderActorModel") return n.attr_or("value", std::string());
    return {};
}

float placeholder_opacity(const data::Catalog& cat, const std::string& actor, float fallback) {
    if (actor.empty()) return fallback;
    float out = fallback;
    for (const core::XmlNode& n : cat.resolved("CActorModel", actor)) {
        if (n.name != "On") continue;
        const std::vector<std::string> terms = split_terms(n.attr_or("Terms", std::string()));
        const std::string send = n.attr_or("Send", std::string());
        if (terms.size() != 1 || terms.front() != "ActorCreation" || send.rfind("SetOpacity ", 0) != 0) continue;
        const float v = first_float(trim(send.substr(11)), -1.0f);
        if (v > 0.0f && v <= 1.0f) out = v;
    }
    return out;
}

namespace {

bool order_point(const sim::World& w, const sim::Order& o, float& x, float& y) {
    if (o.target != sim::kNoTarget && o.target < w.units().size() && w.units()[o.target].alive) {
        x = float(w.units()[o.target].x.to_double());
        y = float(w.units()[o.target].y.to_double());
        return true;
    }
    switch (o.type) {
    case sim::OrderType::Move:
    case sim::OrderType::AttackMove:
    case sim::OrderType::Patrol:
    case sim::OrderType::Unload:
    case sim::OrderType::Cast:
        if (o.tx.raw() == 0 && o.ty.raw() == 0) return false;
        x = float(o.tx.to_double());
        y = float(o.ty.to_double());
        return true;
    default:
        return false;
    }
}

int order_kind(sim::OrderType t) {
    switch (t) {
    case sim::OrderType::Move: return OrderCueState::kMove;
    case sim::OrderType::Patrol: return OrderCueState::kPatrol;
    case sim::OrderType::AttackMove:
    case sim::OrderType::Attack: return OrderCueState::kAttack;
    default: return OrderCueState::kConfirm;
    }
}

formats::Model cursor_quad(const formats::Model& splat, float extent) {
    const float h = extent * 0.5f;
    formats::Model m;
    m.vertex_count = 4;
    m.positions = {-h, -h, 0.0f, h, -h, 0.0f, h, h, 0.0f, -h, h, 0.0f};
    m.uvs = {0.0f, 0.0f, 1.0f, 0.0f, 1.0f, 1.0f, 0.0f, 1.0f};
    m.indices = {0, 1, 2, 0, 2, 3};
    formats::M3Region r;
    r.num_vertices = 4;
    r.num_indices = 6;
    m.regions.push_back(r);
    m.bounds_min[0] = m.bounds_min[1] = -h;
    m.bounds_max[0] = m.bounds_max[1] = h;
    m.bounds_radius = h * 1.4142136f;
    m.materials = splat.materials;
    m.material_refs.assign(1, formats::M3MaterialRef{1, 0});
    m.region_material.assign(m.regions.size(), 0);
    return m;
}

}

OrderCueState& MeleeGame::cues() {
    if (!cues_) cues_ = std::make_shared<OrderCueState>();
    if (!cues_->loaded && cat_) {
        cues_->confirms = confirmation_actor_specs(*cat_);
        cues_->flash = selection_flash_look(*cat_);
        cues_->path = waypoint_path_look(*cat_);
        cues_->splat_actor_by_abil = cursor_splat_actors(*cat_);
        cues_->loaded = true;
    }
    return *cues_;
}

const formats::Model* MeleeGame::unit_ghost_model(const std::string& unit_id) const {
    if (unit_id.empty() || !cat_ || !models_ || !ui_store_) return nullptr;
    std::string actor = data::unit_actor_id(*cat_, unit_id);
    if (actor.empty()) actor = unit_id;
    std::string pm = cat_->scalar("CActorUnit", actor, "PlacementModel").value_or("");
    for (std::size_t at; (at = pm.find("##unitName##")) != std::string::npos;)
        pm.replace(at, 12, cat_->actor_unit_name(actor, "CActorUnit"));
    const formats::Model* m = pm.empty() ? nullptr : models_->cmodel(*ui_store_, pm);
    if (!m) m = models_->cmodel(*ui_store_, unit_id);
    return m;
}

void MeleeGame::cue_confirm_point(float x, float y, bool attack) {
    static const bool kOld = std::getenv("OMC_ORDER_CONFIRM_OLD") != nullptr;
    if (kOld || !cat_ || !models_ || !ui_store_) return;
    OrderCueState& st = cues();
    const std::uint32_t lead = human_lead_caster();
    const bool air = lead < world_.units().size() && world_.units()[lead].flying;
    const ConfirmActorSpec* spec = pick_confirmation(st.confirms, attack, air);
    if (!spec) spec = pick_confirmation(st.confirms, attack, !air);
    if (!spec) return;
    const formats::Model* m = models_->cmodel(*ui_store_, spec->model_id);
    if (!m) return;
    OrderCueState::Marker& mk = st.marker;
    mk.active = true;
    mk.model = m;
    mk.model_id = spec->model_id;
    mk.x = x;
    mk.y = y;
    mk.z = ground_z_at(x, y);
    mk.start_ms = clock_ms_;
    mk.attack = attack;
    mk.tint = spec->tinted ? spec->tint : std::array<float, 3>{1.0f, 1.0f, 1.0f};
    mk.seq = render::select_sequence(*m, {"Stand", "Birth"}, 0);
    if (mk.seq < 0 && !m->sequences.empty()) mk.seq = 0;
    mk.len_ms = mk.seq >= 0 && std::size_t(mk.seq) < m->sequences.size() ? float(m->sequences[std::size_t(mk.seq)].length_ms()) : 0.0f;
    if (mk.len_ms <= 0.0f) mk.len_ms = 1000.0f;
    ++mk.count;
}

void MeleeGame::cue_confirm_target(std::uint32_t target) {
    static const bool kOld = std::getenv("OMC_ORDER_CONFIRM_OLD") != nullptr;
    if (kOld || !cat_ || target >= world_.units().size() || !world_.units()[target].alive) return;
    OrderCueState& st = cues();
    const sim::Unit& t = world_.units()[target];
    const data::Catalog::SelectionLook look = cat_->selection_look();
    auto to_f = [](const std::array<std::uint8_t, 3>& c) {
        return std::array<float, 3>{float(c[0]) / 255.0f, float(c[1]) / 255.0f, float(c[2]) / 255.0f};
    };
    const bool enemy = t.player >= 0 && t.player != cfg_.human_player && !world_.are_allied(t.player, cfg_.human_player);
    const bool ally = t.player >= 0 && t.player != cfg_.human_player && !enemy;
    OrderCueState::Flash& f = st.target_flash;
    f.active = true;
    f.target = target;
    f.start_ms = clock_ms_;
    f.enemy = enemy;
    f.rgb = to_f(enemy ? look.enemy : ally ? look.ally : look.selected);
    ++f.count;
}

void MeleeGame::cue_after_order(bool have_point, float x, float y, std::uint32_t target) {
    if (target != sim::kNoTarget && target < world_.units().size() && world_.units()[target].alive) {
        cue_confirm_target(target);
        return;
    }
    const std::uint32_t lead = human_lead_caster();
    bool attack = false;
    if (lead < world_.units().size() && !world_.units()[lead].orders.empty()) {
        const sim::Order& o = world_.units()[lead].orders.back();
        if (o.target != sim::kNoTarget && o.target < world_.units().size() && world_.units()[o.target].alive) {
            cue_confirm_target(o.target);
            return;
        }
        attack = o.type == sim::OrderType::AttackMove || o.type == sim::OrderType::Attack;
    }
    if (have_point) cue_confirm_point(x, y, attack);
}

void MeleeGame::cue_cursor_point(bool have_point, float x, float y) {
    static const bool kOld = std::getenv("OMC_CURSOR_SPLAT_OLD") != nullptr;
    if (!cues_ && (!have_point || kOld)) return;
    OrderCueState& st = cues();
    OrderCueState::Cursor& c = st.cursor;
    c.active = false;
    if (kOld || !have_point || pending_cast_abil_.empty() || !cat_ || !models_ || !ui_store_) return;
    auto it = st.cursor_assets.find(pending_cast_abil_);
    if (it == st.cursor_assets.end()) {
        OrderCueState::CursorAsset a;
        a.spec = ability_cursor_splat(*cat_, st.splat_actor_by_abil, pending_cast_abil_);
        if (a.spec.found) {
            const float extent = float(a.spec.radius) * 2.0f;
            if (const formats::Model* splat = models_->actor_splat(*ui_store_, a.spec.actor)) {
                a.quad = cursor_quad(*splat, extent);
                a.has_quad = true;
            } else if (const formats::Model* mesh = models_->cmodel(*ui_store_, a.spec.model_id)) {
                float ext = 0.0f;
                for (int k = 0; k < 2; ++k) ext = std::max(ext, std::max(std::fabs(mesh->bounds_min[k]), std::fabs(mesh->bounds_max[k])));
                a.mesh = mesh;
                a.mesh_scale = ext > 1e-4f ? float(a.spec.radius) / ext : 1.0f;
            }
        }
        it = st.cursor_assets.emplace(pending_cast_abil_, std::move(a)).first;
    }
    const OrderCueState::CursorAsset& a = it->second;
    if (!a.spec.found || (!a.has_quad && !a.mesh)) return;
    c.active = true;
    if (c.abil != pending_cast_abil_) c.abil = pending_cast_abil_;
    c.x = x;
    c.y = y;
    c.radius = a.spec.radius;
    c.quad = a.has_quad;
    c.model = a.has_quad ? &a.quad : a.mesh;
    c.scale = a.has_quad ? 1.0f : a.mesh_scale;
}

void MeleeGame::cue_build_issued(std::uint32_t worker, sim::Fixed x, sim::Fixed y, const std::string& unit_id) {
    static const bool kOld = std::getenv("OMC_BUILD_PLACEHOLDER_OLD") != nullptr;
    if (kOld || unit_id.empty()) return;
    OrderCueState& st = cues();
    for (OrderCueState::Placeholder& p : st.issued)
        if (p.worker == worker) {
            p.x = float(x.to_double());
            p.y = float(y.to_double());
            p.unit_id = unit_id;
            return;
        }
    OrderCueState::Placeholder p;
    p.worker = worker;
    p.x = float(x.to_double());
    p.y = float(y.to_double());
    p.unit_id = unit_id;
    st.issued.push_back(std::move(p));
}

void MeleeGame::emit_order_cues(std::vector<render::SceneInstance>& scene) {
    if (!cat_ || cfg_.human_player < 0) return;
    static const bool kConfirmOld = std::getenv("OMC_ORDER_CONFIRM_OLD") != nullptr;
    static const bool kCursorOld = std::getenv("OMC_CURSOR_SPLAT_OLD") != nullptr;
    static const bool kPlaceholderOld = std::getenv("OMC_BUILD_PLACEHOLDER_OLD") != nullptr;
    static const bool kPathOld = std::getenv("OMC_WAYPOINT_PATH_OLD") != nullptr;
    OrderCueState& st = cues();
    const std::array<float, 4> team = team_colour_for(cat_, cfg_.human_player, false);
    const std::array<float, 4> emis = team_emissive_for(cat_, cfg_.human_player);

    if (!kConfirmOld && st.marker.active && st.marker.model) {
        const double el = clock_ms_ - st.marker.start_ms;
        if (el < 0.0 || el > double(st.marker.len_ms)) {
            st.marker.active = false;
        } else {
            render::SceneInstance si;
            si.model = st.marker.model;
            si.world = render::translation(st.marker.x, st.marker.y, st.marker.z);
            si.seq = st.marker.seq;
            si.time_ms = float(el);
            si.tint = st.marker.tint;
            si.team = team;
            si.team_emissive = emis;
            scene.push_back(si);
        }
    }

    if (!kConfirmOld && st.target_flash.active) {
        const double el = clock_ms_ - st.target_flash.start_ms;
        const std::uint32_t t = st.target_flash.target;
        if (el < 0.0 || el > double(st.flash.hold_s) * 1000.0 || t >= world_.units().size() || !world_.units()[t].alive) {
            st.target_flash.active = false;
        } else {
            if (!st.flash_ring_built) {
                st.flash_ring = render::make_arc_ring_model(1.0f - st.flash.inner_width, st.flash.segments, st.flash.solid);
                st.flash_ring_built = true;
            }
            const sim::Unit& u = world_.units()[t];
            const float ux = float(u.x.to_double()), uy = float(u.y.to_double());
            const float body = float(u.radius.to_double());
            const float authored = float(cat_->scalar_num("CUnit", u.catalog_id, "Radius").value_or(0.0));
            const float rr = (authored > 1e-3f ? authored : body) + st.flash.outer_width;
            const float uz = u.flying ? air_z_at(ux, uy) : ground_z_at(ux, uy);
            const float spin = float(el * 0.001) * st.flash.rotation;
            render::SceneInstance ring;
            ring.model = &st.flash_ring;
            ring.world = render::translation(ux, uy, uz + 0.06f) * render::rotation_z(spin) * render::scaling(rr, rr, 1.0f);
            ring.team = {st.target_flash.rgb[0], st.target_flash.rgb[1], st.target_flash.rgb[2], 2.0f};
            ring.seq = -1;
            ring.opacity = st.flash.alpha;
            scene.push_back(ring);
        }
    }

    st.cursor_drawn = 0;
    if (!kCursorOld && st.cursor.active && st.cursor.model && st.cursor.abil == pending_cast_abil_) {
        const float cx = st.cursor.x, cy = st.cursor.y, r = float(st.cursor.radius);
        float cz = ground_z_at(cx, cy);
        cz = std::max(cz, ground_z_at(cx + r, cy));
        cz = std::max(cz, ground_z_at(cx - r, cy));
        cz = std::max(cz, ground_z_at(cx, cy + r));
        cz = std::max(cz, ground_z_at(cx, cy - r));
        render::SceneInstance si;
        si.model = st.cursor.model;
        si.world = render::translation(cx, cy, cz + 0.05f) * render::scaling(st.cursor.scale, st.cursor.scale, st.cursor.scale);
        si.seq = -1;
        si.time_ms = float(clock_ms_);
        si.team = st.cursor.quad ? std::array<float, 4>{0.0f, 0.0f, 0.0f, -1.0f} : team;
        scene.push_back(si);
        st.cursor_drawn = 1;
    }

    st.placeholders_drawn = 0;
    if (!kPlaceholderOld) {
        for (std::size_t i = 0; i < st.issued.size();) {
            const OrderCueState::Placeholder& p = st.issued[i];
            const bool live = p.worker < world_.units().size() && world_.units()[p.worker].alive &&
                              world_.units()[p.worker].player == cfg_.human_player && world_.worker_committed(p.worker) &&
                              world_.units()[p.worker].construct_target == sim::kNoTarget;
            if (!live) {
                st.issued.erase(st.issued.begin() + std::ptrdiff_t(i));
                continue;
            }
            ++i;
        }
        auto ghost = [&](const std::string& unit_id, float x, float y) {
            auto it = st.ghosts.find(unit_id);
            if (it == st.ghosts.end()) {
                OrderCueState::GhostLook g;
                g.model = unit_ghost_model(unit_id);
                g.scale = data::resolve_unit_model_scale(*cat_, data::unit_actor_id(*cat_, unit_id));
                g.face = cat_->unit_facing_radians(unit_id);
                g.opacity = placeholder_opacity(*cat_, placeholder_actor(*cat_, unit_id), 0.5f);
                if (g.model) g.seq = render::select_sequence(*g.model, {"Stand"}, 0);
                it = st.ghosts.emplace(unit_id, g).first;
            }
            const OrderCueState::GhostLook& g = it->second;
            if (!g.model) return;
            render::SceneInstance si;
            si.model = g.model;
            si.world = render::translation(x, y, ground_z_at(x, y)) * render::rotation_z(g.face + 1.5707963f) *
                       render::scaling(g.scale[0], g.scale[1], g.scale[2]);
            si.seq = g.seq;
            si.time_ms = 0.0f;
            si.team = team;
            si.team_emissive = emis;
            si.opacity = g.opacity;
            scene.push_back(si);
            ++st.placeholders_drawn;
        };
        for (const OrderCueState::Placeholder& p : st.issued) ghost(p.unit_id, p.x, p.y);
        for (const QueuedBuild& q : build_queue_) {
            if (q.worker >= world_.units().size() || !world_.units()[q.worker].alive ||
                world_.units()[q.worker].player != cfg_.human_player)
                continue;
            ghost(q.proto.catalog_id, float(q.x.to_double()), float(q.y.to_double()));
        }
    }

    st.path_markers_drawn = 0;
    if (!kPathOld && models_ && ui_store_) {
        constexpr std::size_t kMaxMarkers = 96;
        for (std::uint32_t id : selection_) {
            if (st.path_markers_drawn >= kMaxMarkers) break;
            if (id >= world_.units().size()) continue;
            const sim::Unit& u = world_.units()[id];
            if (!u.alive || u.player != cfg_.human_player || u.orders.empty() || u.catalog_id.empty()) continue;
            auto lit = st.looks.find(u.catalog_id);
            if (lit == st.looks.end()) {
                std::array<OrderCueState::Look, OrderCueState::kKinds> looks;
                const std::string race = cat_->scalar("CUnit", u.catalog_id, "Race").value_or(std::string());
                const char* tags[OrderCueState::kKinds] = {"CAbilMove", "CAbilMove", "CAbilAttack", "CAbil"};
                const int idx[OrderCueState::kKinds] = {0, 1, 0, 0};
                for (int k = 0; k < OrderCueState::kKinds; ++k) {
                    const OrderDisplay d = ability_order_display(*cat_, u.catalog_id, tags[k], idx[k]);
                    OrderCueState::Look& lk = looks[std::size_t(k)];
                    lk.resolved = true;
                    if (d.found) lk.rgb = {float(d.argb[1]) / 255.0f, float(d.argb[2]) / 255.0f, float(d.argb[3]) / 255.0f};
                    const std::string key = d.display_type + "|" + race;
                    auto hit = st.harness.find(key);
                    if (hit == st.harness.end()) {
                        const std::string mid = command_harness_model(*cat_, d.display_type, race);
                        hit = st.harness.emplace(key, mid.empty() ? nullptr : models_->cmodel(*ui_store_, mid)).first;
                    }
                    lk.marker = hit->second;
                    if (lk.marker) {
                        lk.seq = render::select_sequence(*lk.marker, {"Stand"}, 0);
                        if (lk.seq >= 0 && std::size_t(lk.seq) < lk.marker->sequences.size())
                            lk.len_ms = float(lk.marker->sequences[std::size_t(lk.seq)].length_ms());
                    }
                }
                lit = st.looks.emplace(u.catalog_id, looks).first;
            }
            for (const sim::Order& o : u.orders) {
                if (st.path_markers_drawn >= kMaxMarkers) break;
                float tx = 0.0f, ty = 0.0f;
                if (o.target != sim::kNoTarget || !order_point(world_, o, tx, ty)) continue;
                const OrderCueState::Look& lk = lit->second[std::size_t(order_kind(o.type))];
                if (!lk.marker) continue;
                render::SceneInstance si;
                si.model = lk.marker;
                si.world = render::translation(tx, ty, ground_z_at(tx, ty));
                si.seq = lk.seq;
                si.time_ms = lk.len_ms > 0.0f ? float(std::fmod(clock_ms_, double(lk.len_ms))) : 0.0f;
                si.team = team;
                si.team_emissive = emis;
                scene.push_back(si);
                ++st.path_markers_drawn;
            }
        }
    }
}

void MeleeGame::draw_order_paths(std::uint32_t view_w, std::uint32_t view_h, std::vector<std::uint8_t>& out,
                                 const Control& ctrl, const ConsolePanel* clip) {
    if (!cat_ || cfg_.human_player < 0 || view_w == 0 || view_h == 0) return;
    OrderCueState& st = cues();
    const PathLook& pl = st.path;
    st.path_legs_drawn = 0;
    const IconAtlas::Icon* tex = ui_store_ ? &icons_.get(*ui_store_, pl.texture) : nullptr;
    if (tex && !tex->ok()) tex = nullptr;
    constexpr float kLineStrength = 0.6f;
    static const bool kChevronOld = std::getenv("OMC_WAYPOINT_CHEVRON_OLD") != nullptr;
    const render::Vec3 right = camctl_.ground_right();
    auto px_per_world = [&](float x, float y) -> float {
        float ax = 0, ay = 0, bx = 0, by = 0;
        if (!ctrl.project_on_ground(x, y, ax, ay) || !ctrl.project_on_ground(x + right.x, y + right.y, bx, by)) return 0.0f;
        return std::hypot(bx - ax, by - ay);
    };
    auto seg = [&](float ax, float ay, float bx, float by, float half, float tile, const std::array<float, 3>& tint,
                   float fade_alpha, float fade_point) {
        const float dx = bx - ax, dy = by - ay, len = std::hypot(dx, dy);
        if (len < 1.0f || half <= 0.0f) return;
        const float ux = dx / len, uy = dy / len;
        const int y0 = std::max(0, int(std::floor(std::min(ay, by) - half - 1.0f)));
        const int y1 = std::min(int(view_h) - 1, int(std::ceil(std::max(ay, by) + half + 1.0f)));
        const float tint255[3] = {tint[0] * 255.0f, tint[1] * 255.0f, tint[2] * 255.0f};
        for (int y = y0; y <= y1; ++y) {
            const float fy = float(y) + 0.5f - ay;
            float lo = -1e9f, hi = 1e9f;
            auto bound = [&](float coef, float c, float mn, float mx) {
                if (std::fabs(coef) < 1e-6f) {
                    if (c < mn || c > mx) {
                        lo = 1.0f;
                        hi = 0.0f;
                    }
                    return;
                }
                float a = (mn - c) / coef, b = (mx - c) / coef;
                if (a > b) std::swap(a, b);
                lo = std::max(lo, a);
                hi = std::min(hi, b);
            };
            bound(ux, fy * uy, 0.0f, len);
            bound(-uy, fy * ux, -half, half);
            if (lo > hi) continue;
            const int xs = std::max(0, int(std::ceil(lo + ax - 0.5f)));
            const int xe = std::min(int(view_w) - 1, int(std::floor(hi + ax - 0.5f)));
            for (int x = xs; x <= xe; ++x) {
                const float fx = float(x) + 0.5f - ax;
                const float along = fx * ux + fy * uy, across = -fx * uy + fy * ux;
                float strength = kLineStrength;
                if (fade_alpha < 1.0f) {
                    const float f = along / len;
                    strength *= f >= fade_point || fade_point <= 0.0f ? 1.0f : fade_alpha + (1.0f - fade_alpha) * f / fade_point;
                }
                float tr = 1.0f, tg = 1.0f, tb = 1.0f, ta = 1.0f;
                if (tex) {
                    const int sx = std::min(int(tex->w) - 1, std::max(0, int((across + half) / (2.0f * half) * float(tex->w))));
                    const float ph = tile > 0.5f ? std::fmod(len - along, tile) / tile : 0.0f;
                    const float v = kChevronOld ? ph : 1.0f - ph;
                    const int sy = std::min(int(tex->h) - 1, std::max(0, int(v * float(tex->h))));
                    const std::size_t si = (std::size_t(sy) * tex->w + std::size_t(sx)) * 4;
                    tr = float(tex->rgba[si]) / 255.0f;
                    tg = float(tex->rgba[si + 1]) / 255.0f;
                    tb = float(tex->rgba[si + 2]) / 255.0f;
                    ta = float(tex->rgba[si + 3]) / 255.0f;
                }
                const float a = strength * ta;
                if (a <= 0.0f) continue;
                if (clip && hud_covers_point(view_w, view_h, clip, float(x), float(y))) continue;
                const std::size_t di = (std::size_t(y) * view_w + std::size_t(x)) * 4;
                const float src[3] = {tr * tint255[0], tg * tint255[1], tb * tint255[2]};
                if (out[di + 3] == 0) {
                    for (int k = 0; k < 3; ++k) out[di + std::size_t(k)] = std::uint8_t(std::min(255.0f, src[k]) + 0.5f);
                    out[di + 3] = std::uint8_t(a * 255.0f + 0.5f);
                    continue;
                }
                for (int k = 0; k < 3; ++k)
                    out[di + std::size_t(k)] = std::uint8_t(std::min(255.0f, src[k] * a + float(out[di + std::size_t(k)])) + 0.5f);
                out[di + 3] = std::uint8_t(std::max(float(out[di + 3]), a * 255.0f));
            }
        }
    };
    auto leg = [&](float wx0, float wy0, float wx1, float wy1, const std::array<float, 3>& tint, float fade_alpha,
                   float fade_point, float* half_out) {
        float ax = 0, ay = 0, bx = 0, by = 0;
        if (!ctrl.project_on_ground(wx0, wy0, ax, ay) || !ctrl.project_on_ground(wx1, wy1, bx, by)) return;
        const float k = px_per_world(0.5f * (wx0 + wx1), 0.5f * (wy0 + wy1));
        const float wl = std::hypot(wx1 - wx0, wy1 - wy0), sl = std::hypot(bx - ax, by - ay);
        if (k <= 0.0f || wl < 1e-3f) return;
        const float half = std::max(0.5f, 0.5f * pl.width * k);
        const float tile = pl.tile * sl / wl;
        seg(ax, ay, bx, by, half, tile, tint, fade_alpha, fade_point);
        if (half_out) *half_out = half;
        ++st.path_legs_drawn;
    };

    int pathed = 0;
    for (std::uint32_t id : selection_) {
        if (id >= world_.units().size()) continue;
        const sim::Unit& u = world_.units()[id];
        if (u.alive && u.player == cfg_.human_player && !u.orders.empty()) ++pathed;
    }
    const bool multi = pathed > 1;
    for (std::uint32_t id : selection_) {
        if (id >= world_.units().size()) continue;
        const sim::Unit& u = world_.units()[id];
        if (!u.alive || u.player != cfg_.human_player || u.orders.empty() || u.catalog_id.empty()) continue;
        const auto lit = st.looks.find(u.catalog_id);
        float px = float(u.x.to_double()), py = float(u.y.to_double());
        int legs = 0;
        for (const sim::Order& o : u.orders) {
            if (legs >= 16) break;
            float tx = 0.0f, ty = 0.0f;
            if (!order_point(world_, o, tx, ty)) continue;
            const int kind = order_kind(o.type);
            const std::array<float, 3> rgb = lit != st.looks.end() ? lit->second[std::size_t(kind)].rgb
                                                                    : std::array<float, 3>{0.0f, 1.0f, 0.0f};
            leg(px, py, tx, ty, rgb, multi && legs == 0 ? pl.fade_alpha : 1.0f, pl.fade_point, &st.last_path_half_px);
            px = tx;
            py = ty;
            ++legs;
        }
    }
    for (std::uint32_t id : selection_) {
        if (id >= world_.units().size()) continue;
        const sim::Unit& u = world_.units()[id];
        if (!u.alive || u.player != cfg_.human_player || !u.has_rally || u.catalog_id.empty()) continue;
        auto rit = st.rally_rgb.find(u.catalog_id);
        if (rit == st.rally_rgb.end()) {
            std::array<float, 3> c = {200.0f / 255.0f, 200.0f / 255.0f, 200.0f / 255.0f};
            const std::string ab = cat_->unit_rally_ability(u.catalog_id);
            if (!ab.empty())
                if (auto col = cat_->ability_rally_color(ab))
                    c = {float((*col)[0]) / 255.0f, float((*col)[1]) / 255.0f, float((*col)[2]) / 255.0f};
            rit = st.rally_rgb.emplace(u.catalog_id, c).first;
        }
        leg(float(u.x.to_double()), float(u.y.to_double()), float(u.rally_x.to_double()), float(u.rally_y.to_double()),
            rit->second, 1.0f, 0.0f, &st.last_rally_half_px);
    }
}

}
