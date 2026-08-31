#include "sc2/ui/layout.hpp"

#include <cstdlib>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include "sc2/ui/font.hpp"

namespace sc2::ui {

using core::XmlNode;

const XmlNode* Frame::prop(const std::string& tag) const {
    for (const XmlNode& p : props)
        if (p.name == tag) return &p;
    return nullptr;
}

std::size_t Frame::count() const {
    std::size_t n = 1;
    for (const Frame& c : children) n += c.count();
    return n;
}

const Frame* Frame::find(const std::string& name) const {
    for (const Frame& c : children) {
        if (c.name == name) return &c;
        if (const Frame* f = c.find(name)) return f;
    }
    return nullptr;
}

const std::string* Layout::constant(const std::string& name) const {
    for (const auto& [k, v] : constants)
        if (k == name) return &v;
    return nullptr;
}

std::string Layout::resolve(const std::string& value) const {
    if (value.size() >= 2 && value[0] == '#' && value[1] == '#')
        return value.substr(1);
    if (value.size() >= 2 && value[0] == '#') {
        if (const std::string* v = constant(value.substr(1))) return *v;
    }
    return value;
}

std::size_t Layout::frame_count() const {
    std::size_t n = 0;
    for (const Frame& f : frames) n += f.count();
    return n;
}

const Frame* Layout::find(const std::string& name) const {
    for (const Frame& f : frames) {
        if (f.name == name) return &f;
        if (const Frame* c = f.find(name)) return c;
    }
    return nullptr;
}

namespace {
Frame build_frame(const XmlNode& node) {
    Frame f;
    f.type = node.attr_or("type", "");
    f.name = node.attr_or("name", "");
    f.tmpl = node.attr_or("template", "");
    for (const XmlNode& c : node.children) {
        if (c.name == "Frame")
            f.children.push_back(build_frame(c));
        else
            f.props.push_back(c);
    }
    return f;
}
}

Layout parse_layout(const std::string& xml) {
    XmlNode root = core::xml_parse(xml);
    if (root.name != "Desc")
        throw std::runtime_error("layout: root element is not <Desc>");

    Layout layout;
    for (const XmlNode& c : root.children) {
        if (c.name == "Constant") {
            const std::string* n = c.attr("name");
            if (n) layout.constants.emplace_back(*n, c.attr_or("val", ""));
        } else if (c.name == "DescFlags") {
            if (c.attr_or("val", "") == "Locked") layout.locked = true;
        } else if (c.name == "Frame") {
            layout.frames.push_back(build_frame(c));
        }
    }
    return layout;
}

Layout parse_layout(const std::vector<uint8_t>& xml) {
    return parse_layout(std::string(xml.begin(), xml.end()));
}

std::string desc_name_of(const std::string& path) {
    std::size_t slash = path.find_last_of("/\\");
    std::size_t start = slash == std::string::npos ? 0 : slash + 1;
    std::size_t dot = path.find_last_of('.');
    if (dot == std::string::npos || dot < start) return path.substr(start);
    return path.substr(start, dot - start);
}

namespace {
std::vector<std::string> split_path(const std::string& s) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == '/') { if (!cur.empty()) out.push_back(cur); cur.clear(); }
        else cur += c;
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

bool prop_overrides(const XmlNode& a, const XmlNode& b) {
    if (a.name != b.name) return false;
    if (a.name == "Anchor") return a.attr_or("side", "") == b.attr_or("side", "");
    return true;
}

void overlay(Frame& dst, const Frame& src) {
    if (!src.type.empty()) dst.type = src.type;
    for (const XmlNode& p : src.props) {
        bool replaced = false;
        for (XmlNode& q : dst.props)
            if (prop_overrides(q, p)) { q = p; replaced = true; break; }
        if (!replaced) dst.props.push_back(p);
    }
    for (const Frame& c : src.children) {
        Frame* match = nullptr;
        if (!c.name.empty())
            for (Frame& dc : dst.children)
                if (dc.name == c.name) { match = &dc; break; }
        if (match) overlay(*match, c);
        else dst.children.push_back(c);
    }
}
}

std::vector<std::string> LayoutSet::parse_includes(const std::string& xml) {
    std::vector<std::string> out;
    XmlNode root = core::xml_parse(xml);
    for (const XmlNode& c : root.children)
        if (c.name == "Include")
            if (const std::string* p = c.attr("path")) out.push_back(*p);
    return out;
}

void LayoutSet::add(const std::string& path, Layout layout) {
    docs.push_back({desc_name_of(path), path, std::move(layout)});
}

void LayoutSet::add_xml(const std::string& path, const std::string& xml) {
    add(path, parse_layout(xml));
}

const std::string* LayoutSet::constant(const std::string& name) const {
    for (auto it = docs.rbegin(); it != docs.rend(); ++it)
        if (const std::string* v = it->layout.constant(name)) return v;
    return nullptr;
}

std::string LayoutSet::resolve(const std::string& value) const {
    if (value.size() >= 2 && value[0] == '#' && value[1] == '#')
        return value.substr(1);
    if (value.size() >= 2 && value[0] == '#')
        if (const std::string* v = constant(value.substr(1))) return *v;
    return value;
}

const Frame* LayoutSet::find_template(const std::string& path) const {
    std::vector<std::string> seg = split_path(path);
    if (seg.size() < 2) return nullptr;
    const Layout* doc = nullptr;
    for (auto it = docs.rbegin(); it != docs.rend(); ++it)
        if (it->name == seg[0]) { doc = &it->layout; break; }
    if (!doc) return nullptr;
    const Frame* cur = nullptr;
    for (const Frame& f : doc->frames)
        if (f.name == seg[1]) { cur = &f; break; }
    for (std::size_t k = 2; k < seg.size() && cur; ++k) {
        const Frame* next = nullptr;
        for (const Frame& c : cur->children)
            if (c.name == seg[k]) { next = &c; break; }
        cur = next;
    }
    return cur;
}

namespace {
constexpr int kInstantiateBudget = 50000;

Frame instantiate_impl(const LayoutSet& set, const Frame& f,
                       std::unordered_set<std::string>& active, int& budget) {
    if (budget <= 0) return Frame{};
    --budget;
    Frame base;
    if (!f.tmpl.empty() && !active.count(f.tmpl)) {
        if (const Frame* t = set.find_template(f.tmpl)) {
            active.insert(f.tmpl);
            base = instantiate_impl(set, *t, active, budget);
            active.erase(f.tmpl);
        }
    }
    if (!f.type.empty()) base.type = f.type;
    if (!f.name.empty()) base.name = f.name;
    base.tmpl = f.tmpl;
    for (const XmlNode& p : f.props) {
        bool replaced = false;
        for (XmlNode& q : base.props)
            if (prop_overrides(q, p)) { q = p; replaced = true; break; }
        if (!replaced) base.props.push_back(p);
    }
    for (const Frame& c : f.children) {
        if (budget <= 0) break;
        Frame rc = instantiate_impl(set, c, active, budget);
        Frame* match = nullptr;
        if (!rc.name.empty())
            for (Frame& bc : base.children)
                if (bc.name == rc.name) { match = &bc; break; }
        if (match) overlay(*match, rc);
        else base.children.push_back(std::move(rc));
    }
    return base;
}
}

Frame LayoutSet::instantiate(const Frame& f) const {
    std::unordered_set<std::string> active;
    int budget = kInstantiateBudget;
    return instantiate_impl(*this, f, active, budget);
}

namespace {
float anchor_base(const Rect& p, bool horizontal, const std::string& pos) {
    float lo = horizontal ? p.x : p.y;
    float span = horizontal ? p.w : p.h;
    if (pos == "Max") return lo + span;
    if (pos == "Mid") return lo + span * 0.5f;
    return lo;
}

float attr_f(const XmlNode& n, const char* key, float dflt) {
    if (const std::string* v = n.attr(key)) return float(std::atof(v->c_str()));
    return dflt;
}

std::string sibling_ref_name(const std::string& rel) {
    if (rel.empty() || rel == "$parent" || rel == "$this") return "";
    std::size_t slash = rel.find('/');
    if (slash == std::string::npos) return rel;
    if (rel.compare(0, slash, "$parent") == 0 && rel.find('/', slash + 1) == std::string::npos)
        return rel.substr(slash + 1);
    return "";
}

int ancestor_depth(const std::string& rel) {
    if (rel.empty()) return 0;
    int depth = 0;
    std::size_t i = 0;
    while (true) {
        std::size_t slash = rel.find('/', i);
        std::size_t end = (slash == std::string::npos) ? rel.size() : slash;
        if (rel.compare(i, end - i, "$parent") != 0) return 0;
        ++depth;
        if (slash == std::string::npos) break;
        i = slash + 1;
    }
    return depth;
}

const Frame* frame_child(const Frame* f, const std::string& name) {
    if (!f) return nullptr;
    for (const Frame& c : f->children)
        if (c.name == name) return &c;
    return nullptr;
}

const Frame* walk_ref(const Frame* self, const std::vector<const Frame*>& ancestorsF,
                      const std::string& rel) {
    std::vector<std::string> seg = split_path(rel);
    if (seg.empty()) return nullptr;
    const Frame* cur = self;
    std::size_t up = 0;
    bool descended = false;
    for (const std::string& s : seg) {
        if (s == "$this") {
            if (descended || up != 0) return nullptr;
            cur = self;
        } else if (s == "$parent") {
            if (descended) return nullptr;
            ++up;
            if (up > ancestorsF.size()) return nullptr;
            cur = ancestorsF[ancestorsF.size() - up];
        } else if (!s.empty() && s[0] == '$') {
            return nullptr;
        } else {
            cur = frame_child(cur, s);
            if (!cur) return nullptr;
            descended = true;
        }
    }
    return cur == self ? nullptr : cur;
}

struct GlobalResolve {
    const std::unordered_map<const Frame*, Rect>* prev;
    std::unordered_map<const Frame*, Rect>* cur;
    const TextResolver* text = nullptr;
    const FrameCollapsed* collapsed = nullptr;
};

bool declares_collapse(const Frame& f) {
    const XmlNode* p = f.prop("CollapseLayout");
    if (!p) return false;
    const std::string v = p->attr_or("val", "");
    return v == "true" || v == "True" || v == "1";
}

Rect collapse_rect(const Rect& r, bool haveL, bool haveR, bool haveT, bool haveB) {
    Rect c = r;
    c.x = (haveR && !haveL) ? r.x + r.w : r.x;
    c.w = 0.0f;
    c.y = (haveB && !haveT) ? r.y + r.h : r.y;
    c.h = 0.0f;
    return c;
}

Rect anchor_target(const std::string& rel, const Rect& parent,
                   const std::unordered_map<std::string, Rect>& siblings,
                   const std::vector<const Rect*>& ancestors, const Frame& self,
                   const std::vector<const Frame*>& ancestorsF, const GlobalResolve* g) {
    if (int d = ancestor_depth(rel)) {
        if (ancestors.empty()) return parent;
        const std::size_t k = static_cast<std::size_t>(d);
        const std::size_t idx = ancestors.size() >= k ? ancestors.size() - k : 0;
        return *ancestors[idx];
    }
    const std::string name = sibling_ref_name(rel);
    if (!name.empty()) {
        auto it = siblings.find(name);
        return it != siblings.end() ? it->second : parent;
    }
    if (g) {
        if (const Frame* tgt = walk_ref(&self, ancestorsF, rel)) {
            auto ic = g->cur->find(tgt);
            if (ic != g->cur->end()) return ic->second;
            if (g->prev) {
                auto ip = g->prev->find(tgt);
                if (ip != g->prev->end()) return ip->second;
            }
        }
    }
    return parent;
}

std::string measurable_text(const Frame& f, const TextResolver* resolver) {
    const XmlNode* tp = f.prop("Text");
    if (!tp) return "";
    std::string raw = tp->attr_or("val", "");
    if (raw.empty()) return "";
    if (resolver && *resolver) return (*resolver)(raw);
    if (raw.size() >= 2 && (raw[0] == '#' || raw[0] == '@') && raw[1] == raw[0]) return raw.substr(1);
    if (raw[0] == '#' || raw[0] == '@') return raw.substr(1);
    return raw;
}

Rect solve_one(const Frame& f, const Rect& parent,
               const std::unordered_map<std::string, Rect>& siblings,
               const std::vector<const Rect*>& ancestors,
               const std::vector<const Frame*>& ancestorsF, const GlobalResolve* g) {
    bool haveL = false, haveR = false, haveT = false, haveB = false;
    bool haveW = false, haveH = false;
    float L = parent.x, R = parent.x + parent.w;
    float T = parent.y, B = parent.y + parent.h;
    float W = 0, H = 0;
    for (const XmlNode& p : f.props) {
        if (p.name == "Width") { W = attr_f(p, "val", 0); haveW = W > 0; }
        else if (p.name == "Height") { H = attr_f(p, "val", 0); haveH = H > 0; }
        else if (p.name == "Anchor") {
            std::string side = p.attr_or("side", "");
            if (side.empty()) continue;
            std::string pos = p.attr_or("pos", "Min");
            float off = attr_f(p, "offset", 0);
            Rect base = anchor_target(p.attr_or("relative", ""), parent, siblings, ancestors, f,
                                      ancestorsF, g);
            bool horiz = (side == "Left" || side == "Right");
            float c = anchor_base(base, horiz, pos) + off;
            if (side == "Left") { L = c; haveL = true; }
            else if (side == "Right") { R = c; haveR = true; }
            else if (side == "Top") { T = c; haveT = true; }
            else if (side == "Bottom") { B = c; haveB = true; }
        }
    }
    if ((!haveW && !(haveL && haveR)) || (!haveH && !(haveT && haveB))) {
        std::string label = measurable_text(f, g ? g->text : nullptr);
        if (!label.empty()) {
            if (!haveW && !(haveL && haveR)) { W = float(measure_text(label, 1)); haveW = W > 0; }
            if (!haveH && !(haveT && haveB)) { H = float(kFontH); haveH = H > 0; }
        }
    }
    Rect r;
    if (haveL && haveR) { r.x = L; r.w = R - L; }
    else if (haveL && haveW) { r.x = L; r.w = W; }
    else if (haveR && haveW) { r.x = R - W; r.w = W; }
    else if (haveW) { r.x = parent.x + (parent.w - W) * 0.5f; r.w = W; }
    else if (haveL) { r.x = L; r.w = parent.x + parent.w - L; }
    else if (haveR) { r.x = parent.x; r.w = R - parent.x; }
    else { r.x = parent.x; r.w = parent.w; }
    if (haveT && haveB) { r.y = T; r.h = B - T; }
    else if (haveT && haveH) { r.y = T; r.h = H; }
    else if (haveB && haveH) { r.y = B - H; r.h = H; }
    else if (haveH) { r.y = parent.y + (parent.h - H) * 0.5f; r.h = H; }
    else if (haveT) { r.y = T; r.h = parent.y + parent.h - T; }
    else if (haveB) { r.y = parent.y; r.h = B - parent.y; }
    else { r.y = parent.y; r.h = parent.h; }
    if (g && g->collapsed && *g->collapsed && declares_collapse(f) && (*g->collapsed)(f))
        return collapse_rect(r, haveL, haveR, haveT, haveB);
    return r;
}

std::string display_text(const Frame& f, const TextResolver* resolver = nullptr) {
    if (const XmlNode* tp = f.prop("Text")) {
        std::string v = tp->attr_or("val", "");
        if (!v.empty() && resolver && *resolver) return (*resolver)(v);
    }
    std::string t;
    if (const XmlNode* tp = f.prop("Text")) t = tp->attr_or("val", "");
    if (t.empty()) t = f.name;
    if (t.empty()) return t;
    if (t.size() >= 2 && (t[0] == '#' || t[0] == '@') && t[1] == t[0])
        return t.substr(1);
    if (t[0] == '#' || t[0] == '@')
        return t.substr(1);
    return t;
}

void propagate_label_text(Frame& f) {
    if (const XmlNode* tp = f.prop("Text")) {
        std::string val = tp->attr_or("val", "");
        if (!val.empty())
            for (Frame& c : f.children)
                if (c.type == "Label" && !c.prop("Text")) {
                    XmlNode t;
                    t.name = "Text";
                    t.attrs.push_back({"val", val});
                    c.props.push_back(t);
                }
    }
    for (Frame& c : f.children) propagate_label_text(c);
}

std::string texture_ref(const Frame& f) {
    if (const XmlNode* t = f.prop("Texture")) return t->attr_or("val", "");
    if (const XmlNode* i = f.prop("Image")) return i->attr_or("val", "");
    return "";
}

std::string visible_ref(const Frame& f) {
    if (const XmlNode* v = f.prop("Visible")) return v->attr_or("val", "");
    return "";
}

std::vector<std::size_t> sibling_solve_order(const std::vector<Frame>& children) {
    const std::size_t n = children.size();
    std::unordered_map<std::string, std::size_t> name_to_index;
    for (std::size_t i = 0; i < n; ++i)
        if (!children[i].name.empty()) name_to_index[children[i].name] = i;
    std::vector<int> indeg(n, 0);
    std::vector<std::vector<std::size_t>> dependents(n);
    for (std::size_t i = 0; i < n; ++i)
        for (const XmlNode& p : children[i].props) {
            if (p.name != "Anchor") continue;
            const std::string rel = p.attr_or("relative", "");
            if (ancestor_depth(rel)) continue;
            const std::string nm = sibling_ref_name(rel);
            if (nm.empty()) continue;
            auto it = name_to_index.find(nm);
            if (it == name_to_index.end() || it->second == i) continue;
            dependents[it->second].push_back(i);
            ++indeg[i];
        }
    std::vector<std::size_t> order;
    order.reserve(n);
    std::vector<bool> placed(n, false);
    for (std::size_t step = 0; step < n; ++step) {
        std::size_t pick = n;
        for (std::size_t k = 0; k < n; ++k)
            if (!placed[k] && indeg[k] == 0) { pick = k; break; }
        if (pick == n) break;
        placed[pick] = true;
        order.push_back(pick);
        for (std::size_t d : dependents[pick])
            if (indeg[d] > 0) --indeg[d];
    }
    for (std::size_t k = 0; k < n; ++k)
        if (!placed[k]) order.push_back(k);
    return order;
}

void emit_tree(const Frame& f, const Rect& rect, int depth, std::vector<SolvedFrame>& out,
               std::vector<const Rect*>& chain, std::vector<const Frame*>& chainF,
               const GlobalResolve* g) {
    if (g && g->cur) (*g->cur)[&f] = rect;
    out.push_back({rect, f.type, f.name, display_text(f, g ? g->text : nullptr), depth,
                   texture_ref(f), visible_ref(f)});
    const std::size_t n = f.children.size();
    if (n == 0) return;
    chain.push_back(&rect);
    chainF.push_back(&f);
    std::unordered_map<std::string, Rect> child_siblings;
    std::vector<Rect> solved(n);
    for (std::size_t idx : sibling_solve_order(f.children)) {
        const Frame& c = f.children[idx];
        Rect cr = solve_one(c, rect, child_siblings, chain, chainF, g);
        solved[idx] = cr;
        if (!c.name.empty()) child_siblings[c.name] = cr;
    }
    for (std::size_t i = 0; i < n; ++i)
        emit_tree(f.children[i], solved[i], depth + 1, out, chain, chainF, g);
    chain.pop_back();
    chainF.pop_back();
}

std::vector<SolvedFrame> solve_pass(const Frame& root, const Rect& canvas, const GlobalResolve& g) {
    std::vector<SolvedFrame> out;
    const std::unordered_map<std::string, Rect> no_siblings;
    const std::vector<const Rect*> no_ancestors;
    const std::vector<const Frame*> no_ancestorsF;
    const Rect root_rect = solve_one(root, canvas, no_siblings, no_ancestors, no_ancestorsF, &g);
    std::vector<const Rect*> chain;
    std::vector<const Frame*> chainF;
    emit_tree(root, root_rect, 0, out, chain, chainF, &g);
    return out;
}

bool rects_equal(const std::vector<SolvedFrame>& a, const std::vector<SolvedFrame>& b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        const Rect& x = a[i].rect;
        const Rect& y = b[i].rect;
        if (x.x != y.x || x.y != y.y || x.w != y.w || x.h != y.h) return false;
    }
    return true;
}
}

namespace {

bool xml_true(const std::string& v) { return v == "true" || v == "True" || v == "1"; }
bool xml_false(const std::string& v) { return v == "false" || v == "False" || v == "0"; }

template <typename FrameT>
FrameT* walk_state_path(FrameT& owner, const std::string& path) {
    if (path.empty() || path == "$this") return &owner;
    FrameT* cur = &owner;
    for (const std::string& seg : split_path(path)) {
        if (seg == "$this") {
            if (cur != &owner) return nullptr;
            continue;
        }
        if (!seg.empty() && seg[0] == '$') return nullptr;
        FrameT* next = nullptr;
        for (auto& c : cur->children)
            if (c.name == seg) { next = &c; break; }
        if (!next) return nullptr;
        cur = next;
    }
    return cur;
}

enum class Tri { False, True, Unknown };

Tri evaluate_when(const Frame& owner, const XmlNode& when, float aspect) {
    const std::string type = when.attr_or("type", "");
    if (type != "Property") return Tri::Unknown;
    const std::string* want = when.attr("Visible");
    if (!want) return Tri::Unknown;
    for (const auto& [k, v] : when.attrs) {
        (void)v;
        if (k != "type" && k != "frame" && k != "Visible") return Tri::Unknown;
    }
    const Frame* target = walk_state_path(owner, when.attr_or("frame", ""));
    if (!target) return Tri::Unknown;
    const bool is = frame_visible_at_aspect(*target, aspect);
    if (xml_true(*want)) return is ? Tri::True : Tri::False;
    if (xml_false(*want)) return is ? Tri::False : Tri::True;
    return Tri::Unknown;
}

void set_frame_prop(Frame& f, const std::string& name, const std::string& val) {
    if (name.empty()) return;
    for (XmlNode& p : f.props) {
        if (p.name.size() != name.size()) continue;
        bool same = true;
        for (std::size_t i = 0; i < name.size() && same; ++i) {
            const char a = p.name[i], b = name[i];
            same = a == b || (a | 0x20) == (b | 0x20);
        }
        if (!same) continue;
        for (auto& [k, v] : p.attrs)
            if (k == "val") { v = val; return; }
        p.attrs.push_back({"val", val});
        return;
    }
    XmlNode n;
    n.name = name;
    if (n.name[0] >= 'a' && n.name[0] <= 'z') n.name[0] = char(n.name[0] - 'a' + 'A');
    n.attrs.push_back({"val", val});
    f.props.push_back(n);
}

void apply_action(Frame& owner, const XmlNode& action) {
    const std::string type = action.attr_or("type", "");
    Frame* target = walk_state_path(owner, action.attr_or("frame", ""));
    if (!target) return;
    if (type == "SetAnchor") {
        const std::string side = action.attr_or("side", "");
        if (side.empty()) return;
        XmlNode a;
        a.name = "Anchor";
        a.attrs.push_back({"side", side});
        a.attrs.push_back({"relative", action.attr_or("relative", "$parent")});
        a.attrs.push_back({"pos", action.attr_or("pos", "Min")});
        a.attrs.push_back({"offset", action.attr_or("offset", "0")});
        for (XmlNode& p : target->props)
            if (p.name == "Anchor" && p.attr_or("side", "") == side) { p = a; return; }
        target->props.push_back(a);
        return;
    }
    if (type == "SetProperty") {
        for (const auto& [k, v] : action.attrs)
            if (k != "type" && k != "frame" && k != "index") set_frame_prop(*target, k, v);
    }
}

}

std::vector<FrameStateGroup> frame_state_groups(const Frame& f) {
    std::vector<FrameStateGroup> out;
    for (const XmlNode& p : f.props) {
        if (p.name != "StateGroup") continue;
        FrameStateGroup g;
        g.name = p.attr_or("name", "");
        for (const XmlNode& c : p.children) {
            if (c.name == "DefaultState") {
                g.default_state = c.attr_or("val", "");
            } else if (c.name == "State") {
                FrameState s;
                s.name = c.attr_or("name", "");
                for (const XmlNode& e : c.children) {
                    if (e.name == "When") s.whens.push_back(e);
                    else if (e.name == "Action") s.actions.push_back(e);
                }
                g.states.push_back(std::move(s));
            }
        }
        out.push_back(std::move(g));
    }
    return out;
}

bool frame_visible_at_aspect(const Frame& f, float aspect) {
    if (f.type == "AspectRatioFrame") {
        if (const XmlNode* mn = f.prop("MinVisibleAspectRatio")) {
            const std::string v = mn->attr_or("val", "");
            if (!v.empty() && aspect < float(std::atof(v.c_str()))) return false;
        }
        if (const XmlNode* mx = f.prop("MaxVisibleAspectRatio")) {
            const std::string v = mx->attr_or("val", "");
            if (!v.empty() && aspect > float(std::atof(v.c_str()))) return false;
        }
        return true;
    }
    const XmlNode* v = f.prop("Visible");
    if (!v) return true;
    const std::string val = v->attr_or("val", "");
    return !xml_false(val);
}

std::string selected_state(const Frame& owner, const FrameStateGroup& group, float aspect) {
    for (const FrameState& s : group.states) {
        if (s.whens.empty()) continue;
        bool all_true = true;
        for (const XmlNode& w : s.whens)
            if (evaluate_when(owner, w, aspect) != Tri::True) { all_true = false; break; }
        if (all_true) return s.name;
    }
    return group.default_state;
}

void apply_state_groups(Frame& root, float aspect) {
    const std::vector<FrameStateGroup> groups = frame_state_groups(root);
    for (const FrameStateGroup& g : groups) {
        const std::string active = selected_state(root, g, aspect);
        if (active.empty()) continue;
        for (const FrameState& s : g.states) {
            if (s.name != active) continue;
            for (const XmlNode& a : s.actions) apply_action(root, a);
            break;
        }
    }
    for (Frame& c : root.children) apply_state_groups(c, aspect);
}

int measure_text(const std::string& text, int scale) {
    if (scale < 1) scale = 1;
    return int(text.size()) * (kFontW + 1) * scale;
}

std::vector<SolvedFrame> solve_layout(const Frame& root, const Rect& canvas) {
    return solve_layout(root, canvas, TextResolver{});
}

std::vector<SolvedFrame> solve_layout(const Frame& root, const Rect& canvas,
                                      const TextResolver& resolve_text) {
    return solve_layout(root, canvas, resolve_text, FrameCollapsed{});
}

std::vector<SolvedFrame> solve_layout(const Frame& root, const Rect& canvas,
                                      const TextResolver& resolve_text,
                                      const FrameCollapsed& collapsed) {
    Frame tree = root;
    apply_state_groups(tree, canvas.h > 0.0f ? canvas.w / canvas.h : 0.0f);
    propagate_label_text(tree);
    constexpr int kMaxPasses = 64;
    std::unordered_map<const Frame*, Rect> prev;
    std::vector<SolvedFrame> out;
    for (int pass = 0; pass < kMaxPasses; ++pass) {
        std::unordered_map<const Frame*, Rect> cur;
        GlobalResolve g{pass == 0 ? nullptr : &prev, &cur, &resolve_text, &collapsed};
        std::vector<SolvedFrame> next = solve_pass(tree, canvas, g);
        const bool stable = pass > 0 && rects_equal(next, out);
        out = std::move(next);
        if (stable) break;
        prev = std::move(cur);
    }
    return out;
}

namespace {
void blit_scaled(std::vector<uint8_t>& img, uint32_t w, uint32_t h,
                 int x0, int y0, int x1, int y1,
                 const std::vector<uint8_t>& src, uint32_t srcW, uint32_t srcH) {
    int dw = x1 - x0, dh = y1 - y0;
    if (dw <= 0 || dh <= 0 || srcW == 0 || srcH == 0) return;
    if (src.size() < std::size_t(srcW) * srcH * 4) return;
    for (int y = y0; y < y1; ++y) {
        if (y < 0 || y >= int(h)) continue;
        int sy = int((std::int64_t(y - y0) * srcH) / dh);
        if (sy < 0) sy = 0; else if (sy >= int(srcH)) sy = int(srcH) - 1;
        for (int x = x0; x < x1; ++x) {
            if (x < 0 || x >= int(w)) continue;
            int sx = int((std::int64_t(x - x0) * srcW) / dw);
            if (sx < 0) sx = 0; else if (sx >= int(srcW)) sx = int(srcW) - 1;
            std::size_t si = (std::size_t(sy) * srcW + sx) * 4;
            std::size_t di = (std::size_t(y) * w + x) * 4;
            float a = src[si + 3] / 255.0f;
            img[di]     = uint8_t(img[di]     * (1 - a) + src[si]     * a);
            img[di + 1] = uint8_t(img[di + 1] * (1 - a) + src[si + 1] * a);
            img[di + 2] = uint8_t(img[di + 2] * (1 - a) + src[si + 2] * a);
        }
    }
}

std::vector<uint8_t> rasterize_impl(const std::vector<SolvedFrame>& frames,
                                    uint32_t w, uint32_t h,
                                    const TextureFetcher* fetch) {
    std::vector<uint8_t> img(std::size_t(w) * h * 4);
    for (std::size_t i = 0; i < img.size(); i += 4) {
        img[i] = 18; img[i + 1] = 20; img[i + 2] = 26; img[i + 3] = 255;
    }
    auto tint = [](const std::string& type, uint8_t& r, uint8_t& g, uint8_t& b) {
        uint32_t hsh = 2166136261u;
        for (char c : type) { hsh ^= uint8_t(c); hsh *= 16777619u; }
        r = uint8_t(70 + (hsh & 0x7F));
        g = uint8_t(70 + ((hsh >> 8) & 0x7F));
        b = uint8_t(70 + ((hsh >> 16) & 0x7F));
    };
    auto blend = [&](int x, int y, uint8_t r, uint8_t g, uint8_t b, float a) {
        if (x < 0 || y < 0 || x >= int(w) || y >= int(h)) return;
        std::size_t i = (std::size_t(y) * w + x) * 4;
        img[i]     = uint8_t(img[i] * (1 - a) + r * a);
        img[i + 1] = uint8_t(img[i + 1] * (1 - a) + g * a);
        img[i + 2] = uint8_t(img[i + 2] * (1 - a) + b * a);
    };
    for (const SolvedFrame& f : frames) {
        int x0 = int(f.rect.x), y0 = int(f.rect.y);
        int x1 = int(f.rect.x + f.rect.w), y1 = int(f.rect.y + f.rect.h);
        if (x1 <= x0 || y1 <= y0) continue;

        bool drew_texture = false;
        if (fetch && *fetch && !f.texture.empty()) {
            uint32_t tw = 0, th = 0;
            std::vector<uint8_t> rgba;
            if ((*fetch)(f.texture, tw, th, rgba) && tw > 0 && th > 0 &&
                rgba.size() >= std::size_t(tw) * th * 4) {
                blit_scaled(img, w, h, x0, y0, x1, y1, rgba, tw, th);
                drew_texture = true;
            }
        }
        if (!drew_texture) {
            uint8_t r, g, b;
            tint(f.type, r, g, b);
            for (int y = y0; y < y1; ++y)
                for (int x = x0; x < x1; ++x) blend(x, y, r, g, b, 0.18f);
            for (int x = x0; x < x1; ++x) { blend(x, y0, r, g, b, 0.9f); blend(x, y1 - 1, r, g, b, 0.9f); }
            for (int y = y0; y < y1; ++y) { blend(x0, y, r, g, b, 0.9f); blend(x1 - 1, y, r, g, b, 0.9f); }
        }

        if (!f.text.empty()) {
            const int inset = 2;
            int scale = (f.rect.h >= float(kFontH * 2 + inset * 2) &&
                         f.rect.w >= float((kFontW + 1) * 2 + inset * 2)) ? 2 : 1;
            int advance = (kFontW + 1) * scale;
            int gx = x0 + inset, gy = y0 + inset;
            for (char ch : f.text) {
                if (gx >= x1) break;
                for (int ry = 0; ry < kFontH; ++ry)
                    for (int rx = 0; rx < kFontW; ++rx) {
                        if (!font_pixel(ch, rx, ry)) continue;
                        for (int sy = 0; sy < scale; ++sy)
                            for (int sx = 0; sx < scale; ++sx) {
                                int px = gx + rx * scale + sx;
                                int py = gy + ry * scale + sy;
                                if (px < x0 || px >= x1 || py < y0 || py >= y1) continue;
                                blend(px, py, 232, 234, 240, 1.0f);
                            }
                    }
                gx += advance;
            }
        }
    }
    return img;
}
}

std::vector<uint8_t> rasterize_layout(const std::vector<SolvedFrame>& frames,
                                      uint32_t w, uint32_t h) {
    return rasterize_impl(frames, w, h, nullptr);
}

std::vector<uint8_t> rasterize_layout(const std::vector<SolvedFrame>& frames,
                                      uint32_t w, uint32_t h,
                                      const TextureFetcher& fetch) {
    return rasterize_impl(frames, w, h, &fetch);
}

}
