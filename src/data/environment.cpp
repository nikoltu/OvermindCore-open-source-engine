#include "sc2/data/environment.hpp"

#include <cstdio>
#include <cstdlib>
#include <exception>

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

static void apply_light_frame(const XmlNode& frame, EnvLight& out) {
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
        const std::string amb = frame.attr_or("AmbientColor", "");
        if (!amb.empty() && parse_vec3(amb, out.ambient_color) == 3) out.has_ambient = true;
        const std::string op = frame.attr_or("OperatorHDR", "");
        if (!op.empty()) out.params["OperatorHDR"] = std::strtod(op.c_str(), nullptr);
        const std::string cz = frame.attr_or("Colorize", "");
        if (!cz.empty()) out.params["Colorize"] = std::strtod(cz.c_str(), nullptr);
    }

    for (const XmlNode& c : frame.children) {
        if (c.name == "Param") {
            const std::string idx = c.attr_or("index", "");
            const std::string v = node_value(c);
            if (v.empty() || idx.empty()) continue;
            out.params[idx] = std::strtod(v.c_str(), nullptr);
            if (idx == "HDRExposure") { out.hdr_exposure = std::strtod(v.c_str(), nullptr); out.has_hdr = true; }
            else if (idx == "HDRWhitePoint") { out.hdr_white_point = std::strtod(v.c_str(), nullptr); out.has_hdr = true; }
        } else if (c.name == "OperatorHDR") {
            const std::string v = node_value(c);
            if (!v.empty()) out.params["OperatorHDR"] = std::strtod(v.c_str(), nullptr);
        } else if (c.name == "Colorize") {
            const std::string v = node_value(c);
            if (!v.empty()) out.params["Colorize"] = std::strtod(v.c_str(), nullptr);
        } else if (c.name == "AmbientColor") {
            if (parse_vec3(node_value(c), out.ambient_color) == 3) out.has_ambient = true;
        } else if (c.name == "DirectionalLight") {
            const std::string which = c.attr_or("index", "");
            if (which == "Key") {
                read_light(c, out.key);
                for (int i = 0; i < 3; ++i) out.key_dir[i] = out.key.direction[i];
                if (!field(c, "Direction").empty()) out.has_key_dir = true;
            } else if (which == "Fill") {
                read_light(c, out.fill);
            } else if (which == "Back") {
                read_light(c, out.back);
            }
        }
    }
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

    apply_light_frame(*frame, out);
    return out;
}

EnvLight resolve_map_environment(const Catalog& cat, const std::string& tileset) {
    EnvLight env = resolve_environment_light(cat, tileset);
    if (!cat.find("CTerrain", tileset)) return env;
    auto num = [&](const char* field, const char* key) {
        if (auto v = cat.scalar("CTerrain", tileset, field)) env.params[key] = std::atof(v->c_str());
    };
    num("FogEnabled", "TerrainFogEnabled");
    num("FogDensity", "TerrainFogDensity");
    num("FogFalloff", "TerrainFogFalloff");
    num("FogStartingHeight", "TerrainFogStartingHeight");
    if (auto c = cat.scalar("CTerrain", tileset, "FogColor")) {
        int a = 0, r = 0, g = 0, b = 0;
        if (std::sscanf(c->c_str(), "%d,%d,%d,%d", &a, &r, &g, &b) == 4) {
            env.params["TerrainFogColorR"] = r;
            env.params["TerrainFogColorG"] = g;
            env.params["TerrainFogColorB"] = b;
        }
    }
    return env;
}

void overlay_map_light(EnvLight& env, const std::vector<uint8_t>& light_xml) {
    if (light_xml.empty() || env.light_id.empty()) return;
    XmlNode doc;
    try {
        doc = core::xml_parse(light_xml);
    } catch (const std::exception&) {
        return;
    }
    const XmlNode* root = doc.name == "Catalog" ? &doc : find_child(doc, "Catalog");
    if (!root) root = &doc;
    for (const XmlNode& l : root->children) {
        if (l.name != "CLight" || l.attr_or("id", "") != env.light_id) continue;
        for (const XmlNode& c : l.children) {
            if (c.name != "ToDInfoArray") continue;
            const std::string idx = c.attr_or("index", "0");
            if (!(idx.empty() || idx == "0")) continue;
            apply_light_frame(c, env);
            env.found = true;
        }
    }
}

void overlay_map_terrain(EnvLight& env, const std::vector<uint8_t>& terrain_xml, const std::string& tileset) {
    if (terrain_xml.empty() || tileset.empty()) return;
    XmlNode doc;
    try {
        doc = core::xml_parse(terrain_xml);
    } catch (const std::exception&) {
        return;
    }
    const XmlNode* root = doc.name == "Catalog" ? &doc : find_child(doc, "Catalog");
    if (!root) root = &doc;
    for (const XmlNode& t : root->children) {
        if (t.name != "CTerrain" || t.attr_or("id", "") != tileset) continue;
        for (const XmlNode& c : t.children) {
            const std::string v = node_value(c);
            if (v.empty()) continue;
            if (c.name == "FogEnabled") env.params["TerrainFogEnabled"] = std::atof(v.c_str());
            else if (c.name == "FogDensity") env.params["TerrainFogDensity"] = std::atof(v.c_str());
            else if (c.name == "FogFalloff") env.params["TerrainFogFalloff"] = std::atof(v.c_str());
            else if (c.name == "FogStartingHeight") env.params["TerrainFogStartingHeight"] = std::atof(v.c_str());
            else if (c.name == "FogColor") {
                int a = 0, r = 0, g = 0, b = 0;
                if (std::sscanf(v.c_str(), "%d,%d,%d,%d", &a, &r, &g, &b) == 4) {
                    env.params["TerrainFogColorR"] = r;
                    env.params["TerrainFogColorG"] = g;
                    env.params["TerrainFogColorB"] = b;
                }
            }
        }
    }
}

std::map<std::string, double> camera_params(const Catalog& cat, const std::string& camera_id) {
    std::map<std::string, double> out;
    if (const std::vector<XmlNode>* defs = cat.tag_default_indexed_nodes("CCamera"))
        for (const XmlNode& c : *defs) {
            if (c.name != "ParamInitial") continue;
            const std::string idx = c.attr_or("index", "");
            const std::string v = node_value(c);
            if (idx.empty() || v.empty()) continue;
            out[idx] = std::strtod(v.c_str(), nullptr);
        }
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
            else if (vi == "Emissive") parse_rgba(val, tc.emissive);
            else if (vi == "Attacked") parse_rgba(val, tc.attacked);
        }
        bool merged = false;
        for (TeamColor& e : out)
            if (e.slot == slot) { e = tc; merged = true; break; }
        if (!merged) out.push_back(tc);
    }
    return out;
}

}
