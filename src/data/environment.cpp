#include "sc2/data/environment.hpp"

#include <cstdlib>

namespace sc2::data {

using core::XmlNode;

static const XmlNode* find_child(const XmlNode& n, const std::string& name) {
    for (const XmlNode& c : n.children)
        if (c.name == name) return &c;
    return nullptr;
}

static int parse_vec3(const std::string& s, double out[3]) {
    const char* p = s.c_str();
    int i = 0;
    while (i < 3 && *p) {
        char* end = nullptr;
        double v = std::strtod(p, &end);
        if (end == p) break;
        out[i++] = v;
        p = end;
        while (*p == ',' || *p == ' ') ++p;
    }
    return i;
}

static std::string node_value(const XmlNode& n) {
    std::string v = n.attr_or("value", "");
    return v.empty() ? n.text : v;
}

EnvLight resolve_environment_light(const Catalog& cat, const std::string& tileset) {
    EnvLight out;

    if (auto lit = cat.scalar("CTerrain", tileset, "Lighting")) {
        out.light_id = *lit;
        out.resolution = "explicit CTerrain.Lighting";
    } else if (cat.find("CLight", tileset)) {
        out.light_id = tileset;
        out.resolution = "same-id CLight";
    } else {
        out.light_id = "DefaultLight";
        out.resolution = "fallback DefaultLight";
    }
    if (!cat.find("CLight", out.light_id)) return out;
    out.found = true;

    const XmlNode* frame = nullptr;
    for (const XmlNode& c : cat.resolved("CLight", out.light_id)) {
        if (c.name != "ToDInfoArray") continue;
        const std::string idx = c.attr_or("index", "0");
        if (!(idx.empty() || idx == "0")) continue;
        bool populated = !c.children.empty();
        for (const auto& kv : c.attrs)
            if (kv.first != "index") populated = true;
        if (populated) frame = &c;
    }
    if (!frame) return out;

    auto field = [](const XmlNode& n, const char* name) -> std::string {
        std::string v = n.attr_or(name, "");
        if (!v.empty()) return v;
        if (const XmlNode* c = find_child(n, name)) return node_value(*c);
        return std::string();
    };
    auto read_light = [&](const XmlNode& n, EnvDirLight& d) {
        d.present = true;
        std::string s;
        if (!(s = field(n, "Color")).empty()) parse_vec3(s, d.color);
        if (!(s = field(n, "SpecularColor")).empty()) parse_vec3(s, d.spec_color);
        if (!(s = field(n, "Direction")).empty()) parse_vec3(s, d.direction);
        if (!(s = field(n, "ColorMultiplier")).empty()) d.color_multiplier = std::strtod(s.c_str(), nullptr);
        if (!(s = field(n, "SpecColorMultiplier")).empty()) d.spec_multiplier = std::strtod(s.c_str(), nullptr);
        if (!(s = field(n, "UseAmbientOcclusion")).empty()) d.use_ambient_occlusion = (s != "0");
    };

    {
        const std::string amb = frame->attr_or("AmbientColor", "");
        if (!amb.empty() && parse_vec3(amb, out.ambient_color) == 3) out.has_ambient = true;
    }

    for (const XmlNode& c : frame->children) {
        if (c.name == "Param") {
            const std::string idx = c.attr_or("index", "");
            const std::string v = node_value(c);
            if (v.empty() || idx.empty()) continue;
            out.params[idx] = std::strtod(v.c_str(), nullptr);
            if (idx == "HDRExposure") { out.hdr_exposure = std::strtod(v.c_str(), nullptr); out.has_hdr = true; }
            else if (idx == "HDRWhitePoint") { out.hdr_white_point = std::strtod(v.c_str(), nullptr); out.has_hdr = true; }
        } else if (c.name == "AmbientColor") {
            if (parse_vec3(node_value(c), out.ambient_color) == 3) out.has_ambient = true;
        } else if (c.name == "DirectionalLight") {
            const std::string which = c.attr_or("index", "");
            if (which == "Key") {
                read_light(c, out.key);
                for (int i = 0; i < 3; ++i) out.key_dir[i] = out.key.direction[i];
                out.has_key_dir = !field(c, "Direction").empty();
            } else if (which == "Fill") {
                read_light(c, out.fill);
            } else if (which == "Back") {
                read_light(c, out.back);
            }
        }
    }
    return out;
}

std::map<std::string, double> camera_params(const Catalog& cat, const std::string& camera_id) {
    std::map<std::string, double> out;
    for (const XmlNode& c : cat.resolved("CCamera", camera_id)) {
        if (c.name != "ParamInitial") continue;
        const std::string idx = c.attr_or("index", "");
        const std::string v = node_value(c);
        if (idx.empty() || v.empty()) continue;
        out[idx] = std::strtod(v.c_str(), nullptr);
    }
    return out;
}

static void parse_rgba(const std::string& s, float out[4]) {
    double v[4];
    int n = parse_vec3(s, v);
    const char* p = s.c_str();
    for (int k = 0; k < 3 && *p; ++k) { while (*p && *p != ',') ++p; if (*p == ',') ++p; }
    if (*p) { char* e = nullptr; double a = std::strtod(p, &e); if (e != p) { v[3] = a; n = 4; } }
    for (int i = 0; i < n && i < 4; ++i) out[i] = float(v[i]);
}

std::vector<TeamColor> team_colors(const Catalog& cat) {
    std::vector<TeamColor> out;
    int pos = 0;
    for (const XmlNode& f : cat.singleton_fields("CGame")) {
        if (f.name != "TeamColors") continue;
        const std::string* idx = f.attr("index");
        int slot = idx ? std::atoi(idx->c_str()) : pos++;
        TeamColor tc;
        tc.slot = slot;
        for (const XmlNode& v : f.children) {
            if (v.name != "Value") continue;
            const std::string vi = v.attr_or("index", "");
            const std::string val = v.attr_or("value", "");
            if (vi == "Diffuse") parse_rgba(val, tc.diffuse);
            else if (vi == "Selected") parse_rgba(val, tc.selected);
        }
        bool merged = false;
        for (TeamColor& e : out)
            if (e.slot == slot) { e = tc; merged = true; break; }
        if (!merged) out.push_back(tc);
    }
    return out;
}

}
