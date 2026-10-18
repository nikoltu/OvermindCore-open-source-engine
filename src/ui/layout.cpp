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

template <typename Find>
static std::string scoped_constant(const std::string& name, int hashes, const ConstScope& scope,
                                   Find find, const std::string& original) {
    if (hashes >= 3 && !scope.skin.empty())
        if (const std::string* v = find(name + "_" + scope.skin)) return *v;
    if (hashes >= 2 && !scope.race.empty())
        if (const std::string* v = find(name + "_" + scope.race)) return *v;
    if (const std::string* v = find(name)) return *v;
    return original;
}

static int hash_depth(const std::string& v) {
    int n = 0;
    while (n < 3 && std::size_t(n) < v.size() && v[std::size_t(n)] == '#') ++n;
    return n;
}

const char* layout_race_suffix(int race) {
    switch (race) {
        case 0: return "Terr";
        case 1: return "Prot";
        case 2: return "Zerg";
        default: return "";
    }
}

std::string Layout::resolve(const std::string& value, const ConstScope& scope) const {
    const int h = hash_depth(value);
    if (h == 0 || value.size() <= std::size_t(h)) return value;
    return scoped_constant(value.substr(std::size_t(h)), h, scope,
                           [this](const std::string& n) { return constant(n); }, value);
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
    const int h = hash_depth(value);
    if (h == 0 || value.size() <= std::size_t(h)) return value;
    return scoped_constant(value.substr(std::size_t(h)), h, scope,
                           [this](const std::string& n) { return constant(n); }, value);
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

namespace {
void expand_numeric_constants(const LayoutSet& set, Frame& f) {
    for (XmlNode& p : f.props) {
        const char* key = nullptr;
        if (p.name == "Anchor") key = "offset";
        else if (p.name == "Width" || p.name == "Height") key = "val";
        if (!key) continue;
        for (auto& kv : p.attrs) {
            if (kv.first != key || kv.second.size() < 2 || kv.second[0] != '#') continue;
            kv.second = set.resolve(kv.second);
        }
    }
    for (Frame& c : f.children) expand_numeric_constants(set, c);
}
}

Frame LayoutSet::instantiate(const Frame& f) const {
    std::unordered_set<std::string> active;
    int budget = kInstantiateBudget;
    Frame out = instantiate_impl(*this, f, active, budget);
    expand_numeric_constants(*this, out);
    return out;
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

std::string normalise_rel(std::string rel) {
    for (char& c : rel)
        if (c == '\\') c = '/';
    return rel;
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
    const StyleHeight* style_height = nullptr;
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

bool relative_is_collapsed(const std::string& rel, const std::vector<const Frame*>& ancestorsF,
                           const GlobalResolve* g) {
    if (!g || !g->collapsed || !*g->collapsed) return false;
    const std::string name = sibling_ref_name(rel);
    if (name.empty() || ancestorsF.empty()) return false;
    const Frame* parent = ancestorsF.back();
    for (const Frame& c : parent->children) {
        if (c.name != name) continue;
        return declares_collapse(c) && (*g->collapsed)(c);
    }
    return false;
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
            if (side.empty()) {
                const std::string rel = normalise_rel(p.attr_or("relative", ""));
                if (rel.empty() || rel == "$parent" || rel == "$this") continue;
                const float off = attr_f(p, "offset", 0);
                const Rect base = anchor_target(rel, parent, siblings, ancestors, f, ancestorsF, g);
                L = base.x + off;
                R = base.x + base.w - off;
                T = base.y + off;
                B = base.y + base.h - off;
                haveL = haveR = haveT = haveB = true;
                continue;
            }
            std::string pos = p.attr_or("pos", "Min");
            const std::string rel = normalise_rel(p.attr_or("relative", ""));
            float off = relative_is_collapsed(rel, ancestorsF, g) ? 0.0f : attr_f(p, "offset", 0);
            Rect base = anchor_target(rel, parent, siblings, ancestors, f, ancestorsF, g);
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
    if (!haveH && !(haveT && haveB) && g && g->style_height && *g->style_height) {
        if (const XmlNode* sp = f.prop("Style")) {
            const int sh = (*g->style_height)(sp->attr_or("val", ""));
            if (sh > 0) { H = float(sh); haveH = true; }
        }
    }
    Rect r;
    if (haveL && haveR && haveW && R - L < 1.0f) { r.x = 0.5f * (L + R) - 0.5f * W; r.w = W; }
    else if (haveL && haveR) { r.x = L; r.w = R - L; }
    else if (haveL && haveW) { r.x = L; r.w = W; }
    else if (haveR && haveW) { r.x = R - W; r.w = W; }
    else if (haveW) { r.x = parent.x + (parent.w - W) * 0.5f; r.w = W; }
    else if (haveL) { r.x = L; r.w = parent.x + parent.w - L; }
    else if (haveR) { r.x = parent.x; r.w = R - parent.x; }
    else { r.x = parent.x; r.w = parent.w; }
    if (haveT && haveB && haveH && B - T < 1.0f) { r.y = 0.5f * (T + B) - 0.5f * H; r.h = H; }
    else if (haveT && haveB) { r.y = T; r.h = B - T; }
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

std::string accepts_mouse_ref(const Frame& f) {
    if (const XmlNode* v = f.prop("AcceptsMouse")) return v->attr_or("val", "");
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
            const std::string rel = normalise_rel(p.attr_or("relative", ""));
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
    out.push_back({rect, f.type, f.name, display_text(f, g ? g->text : nullptr),
                   chainF.empty() ? std::string() : chainF.back()->name, depth, texture_ref(f),
                   visible_ref(f), accepts_mouse_ref(f)});
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

using Tri = WhenAnswer;
}

std::string selected_state_at(const Frame& owner, const FrameStateGroup& group, float aspect,
                              const std::vector<const Frame*>& ancestors, int depth);

namespace {

const Frame* resolve_state_target(const Frame& owner, const std::string& path,
                                  const std::vector<const Frame*>& ancestors,
                                  std::vector<const Frame*>& target_ancestors) {
    target_ancestors = ancestors;
    if (path.empty() || path == "$this") return &owner;
    const std::vector<std::string> segs = split_path(path);
    if (segs.empty()) return &owner;
    const Frame* cur = &owner;
    std::size_t first = 0;
    const std::string& head = segs[0];
    if (!head.empty() && head[0] == '$' && head != "$this") {
        std::size_t idx = ancestors.size();
        if (head == "$parent") {
            if (ancestors.empty()) return nullptr;
            idx = ancestors.size() - 1;
            first = 1;
            while (first < segs.size() && segs[first] == "$parent") {
                if (idx == 0) return nullptr;
                --idx;
                ++first;
            }
        } else if (head.compare(0, 10, "$ancestor[") == 0) {
            const std::size_t eq = head.find("@type=");
            if (eq == std::string::npos) return nullptr;
            const std::string want = head.substr(eq + 6, head.size() - (eq + 6) - (head.back() == ']' ? 1 : 0));
            bool found = false;
            for (std::size_t i = ancestors.size(); i-- > 0;)
                if (ancestors[i]->type == want) { idx = i; found = true; break; }
            if (!found) return nullptr;
            first = 1;
        } else {
            const std::string want = head.substr(1);
            bool found = false;
            if (owner.name == want) { found = true; idx = ancestors.size(); }
            if (!found)
                for (std::size_t i = ancestors.size(); i-- > 0;)
                    if (ancestors[i]->name == want) { idx = i; found = true; break; }
            if (!found) return nullptr;
            first = 1;
        }
        if (idx < ancestors.size()) {
            cur = ancestors[idx];
            target_ancestors.assign(ancestors.begin(), ancestors.begin() + std::ptrdiff_t(idx));
        }
    }
    for (std::size_t i = first; i < segs.size(); ++i) {
        const std::string& seg = segs[i];
        if (seg == "$this") continue;
        if (!seg.empty() && seg[0] == '$') return nullptr;
        const Frame* next = nullptr;
        for (const Frame& c : cur->children)
            if (c.name == seg) { next = &c; break; }
        if (!next) return nullptr;
        target_ancestors.push_back(cur);
        cur = next;
    }
    return cur;
}

Tri evaluate_when_at(const Frame& owner, const XmlNode& when, float aspect,
                     const std::vector<const Frame*>& ancestors, int depth) {
    const std::string type = when.attr_or("type", "");
    if (type == "StateGroup") {
        if (depth > 6) return Tri::Unknown;
        std::string group, state;
        for (const auto& [k, v] : when.attrs) {
            if (k == "type" || k == "frame") continue;
            if (!group.empty()) return Tri::Unknown;
            group = k;
            state = v;
        }
        if (group.empty()) return Tri::Unknown;
        std::vector<const Frame*> target_chain;
        const Frame* target = resolve_state_target(owner, when.attr_or("frame", ""), ancestors, target_chain);
        if (!target) return Tri::Unknown;
        for (const FrameStateGroup& g : frame_state_groups(*target)) {
            if (g.name != group) continue;
            std::string active;
            bool unknown_seen = false;
            for (const FrameState& st : g.states) {
                if (st.whens.empty()) continue;
                bool all_true = true, any_unknown = false;
                for (const XmlNode& w : st.whens) {
                    const Tri a = evaluate_when_at(*target, w, aspect, target_chain, depth + 1);
                    if (a == Tri::False) { all_true = false; any_unknown = false; break; }
                    if (a == Tri::Unknown) { all_true = false; any_unknown = true; }
                }
                if (all_true) { active = st.name; break; }
                if (any_unknown) unknown_seen = true;
            }
            if (active.empty()) {
                if (unknown_seen) return Tri::Unknown;
                active = g.default_state;
            }
            return active == state ? Tri::True : Tri::False;
        }
        return Tri::Unknown;
    }
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

std::string selected_state_at(const Frame& owner, const FrameStateGroup& group, float aspect,
                              const std::vector<const Frame*>& ancestors, int depth) {
    for (const FrameState& s : group.states) {
        if (s.whens.empty()) continue;
        bool all_true = true;
        for (const XmlNode& w : s.whens)
            if (evaluate_when_at(owner, w, aspect, ancestors, depth) != Tri::True) { all_true = false; break; }
        if (all_true) return s.name;
    }
    return group.default_state;
}

std::string selected_state(const Frame& owner, const FrameStateGroup& group, float aspect) {
    return selected_state_at(owner, group, aspect, {}, 0);
}

std::string selected_state(const Frame& owner, const FrameStateGroup& group, float aspect,
                           const std::vector<const Frame*>& ancestors) {
    return selected_state_at(owner, group, aspect, ancestors, 0);
}

WhenAnswer evaluate_when(const Frame& owner, const core::XmlNode& when, float aspect,
                         const std::vector<const Frame*>& ancestors) {
    return evaluate_when_at(owner, when, aspect, ancestors, 0);
}

namespace {
void apply_state_groups_at(Frame& root, float aspect, std::vector<const Frame*>& chain) {
    const std::vector<FrameStateGroup> groups = frame_state_groups(root);
    for (const FrameStateGroup& g : groups) {
        const std::string active = selected_state_at(root, g, aspect, chain, 0);
        if (active.empty()) continue;
        for (const FrameState& s : g.states) {
            if (s.name != active) continue;
            for (const XmlNode& a : s.actions) apply_action(root, a);
            break;
        }
    }
    chain.push_back(&root);
    for (Frame& c : root.children) apply_state_groups_at(c, aspect, chain);
    chain.pop_back();
}
}

void apply_state_groups(Frame& root, float aspect) {
    std::vector<const Frame*> chain;
    apply_state_groups_at(root, aspect, chain);
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
    return solve_layout(root, canvas, resolve_text, collapsed, DataResolver{});
}

static void bind_data_properties(Frame& f, const DataResolver& data) {
    for (core::XmlNode& p : f.props) {
        if (p.name != "Visible" && p.name != "Text") continue;
        for (auto& kv : p.attrs) {
            if (kv.first != "val") continue;
            const std::string& v = kv.second;
            if (v.size() < 6 || v[0] != '{' || v[1] != '$' || v.back() != '}') continue;
            const std::size_t at = v.find("/@");
            if (at == std::string::npos) continue;
            const std::string target = v.substr(2, at - 2);
            const std::string property = v.substr(at + 2, v.size() - at - 3);
            if (target.empty() || property.empty()) continue;
            const std::string answer = data(target, property);
            if (!answer.empty()) kv.second = answer;
        }
    }
    for (Frame& c : f.children) bind_data_properties(c, data);
}

std::vector<SolvedFrame> solve_layout(const Frame& root, const Rect& canvas,
                                      const TextResolver& resolve_text,
                                      const FrameCollapsed& collapsed,
                                      const DataResolver& data) {
    return solve_layout(root, canvas, resolve_text, collapsed, data, StyleHeight{});
}

std::vector<SolvedFrame> solve_layout(const Frame& root, const Rect& canvas,
                                      const TextResolver& resolve_text,
                                      const FrameCollapsed& collapsed,
                                      const DataResolver& data,
                                      const StyleHeight& style_height) {
    Frame tree = root;
    apply_state_groups(tree, canvas.h > 0.0f ? canvas.w / canvas.h : 0.0f);
    if (data) bind_data_properties(tree, data);
    propagate_label_text(tree);
    constexpr int kMaxPasses = 64;
    std::unordered_map<const Frame*, Rect> prev;
    std::vector<SolvedFrame> out;
    for (int pass = 0; pass < kMaxPasses; ++pass) {
        std::unordered_map<const Frame*, Rect> cur;
        GlobalResolve g{pass == 0 ? nullptr : &prev, &cur, &resolve_text, &collapsed, &style_height};
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
            const int fy0 = y0 < 0 ? 0 : y0, fy1 = y1 > int(h) ? int(h) : y1;
            const int fx0 = x0 < 0 ? 0 : x0, fx1 = x1 > int(w) ? int(w) : x1;
            for (int y = fy0; y < fy1; ++y)
                for (int x = fx0; x < fx1; ++x) blend(x, y, r, g, b, 0.18f);
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

std::string StyleSet::expand(std::string v) const {
    for (int hops = 0; hops < 16 && !v.empty() && v[0] == '#'; ++hops) {
        auto it = consts_.find(v.substr(1));
        if (it == consts_.end()) break;
        if (it->second == v) break;
        v = it->second;
    }
    return v;
}

void StyleSet::ingest(const std::string& xml) {
    core::XmlNode root;
    try {
        root = core::xml_parse(xml);
    } catch (const std::exception&) {
        return;
    }
    std::vector<const core::XmlNode*> flat;
    for (const core::XmlNode& c : root.children) {
        flat.push_back(&c);
        for (const core::XmlNode& g : c.children) flat.push_back(&g);
    }
    for (const core::XmlNode* e : flat) {
        if (e->name == "Constant") {
            const std::string* n = e->attr("name");
            const std::string* v = e->attr("val");
            if (n && v) consts_[*n] = *v;
        } else if (e->name == "FontGroup") {
            const std::string* n = e->attr("name");
            if (!n) continue;
            std::string file;
            for (const core::XmlNode& cr : e->children) {
                if (cr.name != "CodepointRange") continue;
                const std::string* f = cr.attr("font");
                if (!f || f->empty()) continue;
                std::string cand = expand(*f);
                if (!cand.empty() && cand[0] != '#') file = cand;
            }
            if (!file.empty()) {
                for (char& c : file)
                    if (c == '\\') c = '/';
                font_groups_[*n] = file;
            }
        } else if (e->name == "Style") {
            const std::string* n = e->attr("name");
            if (!n) continue;
            Raw raw;
            for (const auto& kv : e->attrs) raw.attrs.push_back(kv);
            styles_[*n] = std::move(raw);
        }
    }
}

FontStyle StyleSet::resolve(const std::string& name) const {
    FontStyle out;
    out.name = name;
    std::vector<std::pair<std::string, std::string>> merged;
    std::vector<std::string> flag_chain;
    std::string cur = name;
    for (int depth = 0; depth < 16 && !cur.empty(); ++depth) {
        auto it = styles_.find(cur);
        if (it == styles_.end()) break;
        std::string next;
        for (const auto& kv : it->second.attrs) {
            if (kv.first == "template") { next = kv.second; continue; }
            if (kv.first == "styleflags") flag_chain.push_back(kv.second);
            bool have = false;
            for (const auto& m : merged)
                if (m.first == kv.first) { have = true; break; }
            if (!have) merged.push_back(kv);
        }
        cur = next;
    }
    if (merged.empty()) return out;
    auto get = [&](const char* k) -> std::string {
        for (const auto& m : merged)
            if (m.first == k) return expand(m.second);
        return std::string();
    };
    const std::string h = get("height");
    if (!h.empty()) {
        try {
            out.height = std::stoi(h);
        } catch (...) {
            out.height = 0;
        }
    }
    const std::string so = get("shadowoffset");
    if (!so.empty()) {
        try {
            out.shadow_offset = std::stoi(so);
        } catch (...) {
        }
    }
    for (auto lvl = flag_chain.rbegin(); lvl != flag_chain.rend(); ++lvl) {
        const std::string s = expand(*lvl);
        std::size_t i = 0;
        while (i < s.size()) {
            const std::size_t b = s.find_first_not_of(", |\t\r\n", i);
            if (b == std::string::npos) break;
            std::size_t e = s.find_first_of(", |\t\r\n", b);
            if (e == std::string::npos) e = s.size();
            std::string tok = s.substr(b, e - b);
            i = e;
            const bool off = !tok.empty() && tok[0] == '!';
            if (off) tok.erase(0, 1);
            if (tok == "Shadow") out.shadow = !off;
        }
    }
    out.vjustify = get("vjustify");
    out.hjustify = get("hjustify");
    {
        const std::string tc = get("textcolor");
        unsigned comp[4] = {255, 255, 255, 255};
        bool okc = false;
        if (!tc.empty() && tc[0] != '#') {
            if (tc.find(',') != std::string::npos) {
                std::vector<unsigned> vals;
                std::string cur;
                for (char c : tc + ",") {
                    if (c == ',') {
                        try { vals.push_back(unsigned(std::stoul(cur)) & 255u); } catch (...) { vals.clear(); break; }
                        cur.clear();
                    } else if (c != ' ') cur += c;
                }
                if (vals.size() == 3) { comp[0] = 255; comp[1] = vals[0]; comp[2] = vals[1]; comp[3] = vals[2]; okc = true; }
                else if (vals.size() == 4) { comp[0] = vals[0]; comp[1] = vals[1]; comp[2] = vals[2]; comp[3] = vals[3]; okc = true; }
            } else if (tc.size() == 6 || tc.size() == 8) {
                bool hex = true;
                for (char c : tc) hex = hex && std::isxdigit(static_cast<unsigned char>(c));
                if (hex) {
                    const unsigned long v = std::stoul(tc, nullptr, 16);
                    if (tc.size() == 8) comp[0] = unsigned((v >> 24) & 255u);
                    comp[1] = unsigned((v >> 16) & 255u); comp[2] = unsigned((v >> 8) & 255u); comp[3] = unsigned(v & 255u);
                    okc = true;
                }
            }
        }
        if (okc) {
            out.has_color = true;
            out.a = std::uint8_t(comp[0]); out.r = std::uint8_t(comp[1]); out.g = std::uint8_t(comp[2]); out.b = std::uint8_t(comp[3]);
        }
    }
    std::string f = get("font");
    if (!f.empty()) {
        auto g = font_groups_.find(f);
        if (g != font_groups_.end()) f = g->second;
        for (char& c : f)
            if (c == '\\') c = '/';
        if (!f.empty() && f[0] != '#') out.font = f;
    }
    return out;
}

}
