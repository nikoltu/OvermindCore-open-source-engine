#include "sc2/ui/layout.hpp"

#include <cstdlib>
#include <stdexcept>
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

void overlay(Frame& dst, const Frame& src) {
    if (!src.type.empty()) dst.type = src.type;
    for (const XmlNode& p : src.props) {
        bool replaced = false;
        for (XmlNode& q : dst.props)
            if (q.name == p.name) { q = p; replaced = true; break; }
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
            if (q.name == p.name) { q = p; replaced = true; break; }
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

Rect solve_one(const Frame& f, const Rect& parent) {
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
            bool horiz = (side == "Left" || side == "Right");
            float c = anchor_base(parent, horiz, pos) + off;
            if (side == "Left") { L = c; haveL = true; }
            else if (side == "Right") { R = c; haveR = true; }
            else if (side == "Top") { T = c; haveT = true; }
            else if (side == "Bottom") { B = c; haveB = true; }
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
    return r;
}

std::string display_text(const Frame& f) {
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

std::string texture_ref(const Frame& f) {
    if (const XmlNode* t = f.prop("Texture")) return t->attr_or("val", "");
    if (const XmlNode* i = f.prop("Image")) return i->attr_or("val", "");
    return "";
}

void solve_rec(const Frame& f, const Rect& parent, int depth,
               std::vector<SolvedFrame>& out) {
    Rect r = solve_one(f, parent);
    out.push_back({r, f.type, f.name, display_text(f), depth, texture_ref(f)});
    for (const Frame& c : f.children) solve_rec(c, r, depth + 1, out);
}
}

std::vector<SolvedFrame> solve_layout(const Frame& root, const Rect& canvas) {
    std::vector<SolvedFrame> out;
    solve_rec(root, canvas, 0, out);
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
