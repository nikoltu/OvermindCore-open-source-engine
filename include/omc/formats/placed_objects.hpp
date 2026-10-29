#pragma once

#include <cstdlib>
#include <string>
#include <vector>

#include "omc/core/xml.hpp"

namespace omc::formats {

struct PlacedUnit {
    std::string unit_type;
    float x = 0, y = 0, z = 0;
    float rotation = 0;
    int player = 0;
    int variation = -1;
};

struct PlacedDoodad {
    std::string type;
    float x = 0, y = 0, z = 0;
    float rotation = 0;
    float pitch = 0, roll = 0;
    float sx = 1, sy = 1, sz = 1;
    int variation = 0;
    bool no_footprint = false;
    bool height_absolute = false;
    bool has_tint = false;
    float tint[3] = {1.0f, 1.0f, 1.0f};
    float tint_mult = 1.0f;
};

struct PlacedPoint {
    std::string type;
    std::string name;
    float x = 0, y = 0, z = 0;
};

struct PlacedObjects {
    int version = 0;
    std::vector<PlacedUnit> units;
    std::vector<PlacedDoodad> doodads;
    std::vector<PlacedPoint> points;
    std::vector<std::pair<float, float>> start_locations;
};

namespace detail {
inline void parse_vec3_or(const std::string& s, float& x, float& y, float& z, float d) {
    x = y = z = d;
    if (s.empty()) return;
    const char* p = s.c_str();
    char* end = nullptr;
    x = std::strtof(p, &end);
    if (end && *end == ',') y = std::strtof(end + 1, &end);
    if (end && *end == ',') z = std::strtof(end + 1, &end);
}

inline void parse_vec3(const std::string& s, float& x, float& y, float& z) {
    x = y = z = 0.0f;
    const char* p = s.c_str();
    char* end = nullptr;
    x = std::strtof(p, &end);
    if (end && *end == ',') y = std::strtof(end + 1, &end);
    if (end && *end == ',') z = std::strtof(end + 1, &end);
}
}

inline PlacedObjects parse_placed_objects(const std::vector<std::uint8_t>& objects_xml) {
    PlacedObjects out;
    core::XmlNode root = core::xml_parse(objects_xml);

    const core::XmlNode* po = root.name == "PlacedObjects" ? &root : nullptr;
    if (!po)
        for (const auto& c : root.children)
            if (c.name == "PlacedObjects") { po = &c; break; }
    if (!po) po = &root;

    if (const std::string* v = po->attr("Version")) out.version = std::atoi(v->c_str());

    for (const core::XmlNode* u : po->elements("ObjectUnit")) {
        PlacedUnit pu;
        pu.unit_type = u->attr_or("UnitType", "");
        detail::parse_vec3(u->attr_or("Position", ""), pu.x, pu.y, pu.z);
        pu.rotation = std::strtof(u->attr_or("Rotation", "0").c_str(), nullptr);
        pu.player = std::atoi(u->attr_or("Player", "0").c_str());
        if (const std::string* v = u->attr("Variation")) pu.variation = std::atoi(v->c_str());
        out.units.push_back(std::move(pu));
    }
    for (const core::XmlNode* d : po->elements("ObjectDoodad")) {
        PlacedDoodad pd;
        pd.type = d->attr_or("Type", "");
        detail::parse_vec3(d->attr_or("Position", ""), pd.x, pd.y, pd.z);
        pd.rotation = std::strtof(d->attr_or("Rotation", "0").c_str(), nullptr);
        pd.pitch = std::strtof(d->attr_or("Pitch", "0").c_str(), nullptr);
        pd.roll = std::strtof(d->attr_or("Roll", "0").c_str(), nullptr);
        detail::parse_vec3_or(d->attr_or("Scale", ""), pd.sx, pd.sy, pd.sz, 1.0f);
        pd.variation = std::atoi(d->attr_or("Variation", "0").c_str());
        if (const std::string* tc = d->attr("TintColor")) {
            float r = 255.0f, g = 255.0f, b = 255.0f;
            detail::parse_vec3_or(*tc, r, g, b, 255.0f);
            const std::size_t sp = tc->find(' ');
            pd.has_tint = true;
            pd.tint[0] = r / 255.0f; pd.tint[1] = g / 255.0f; pd.tint[2] = b / 255.0f;
            pd.tint_mult = sp != std::string::npos ? std::strtof(tc->c_str() + sp + 1, nullptr) : 1.0f;
        }
        for (const core::XmlNode* f : d->elements("Flag"))
        {
            if (f->attr_or("Index", "") == "NoDoodadFootprint" && f->attr_or("Value", "") != "0")
                pd.no_footprint = true;
            if (f->attr_or("Index", "") == "HeightAbsolute" && f->attr_or("Value", "") != "0")
                pd.height_absolute = true;
        }
        out.doodads.push_back(std::move(pd));
    }
    for (const core::XmlNode* pt : po->elements("ObjectPoint")) {
        PlacedPoint pp;
        pp.type = pt->attr_or("Type", "");
        pp.name = pt->attr_or("Name", "");
        detail::parse_vec3(pt->attr_or("Position", ""), pp.x, pp.y, pp.z);
        if (pp.type == "StartLoc") out.start_locations.emplace_back(pp.x, pp.y);
        out.points.push_back(std::move(pp));
    }
    return out;
}

inline bool is_resource_type(const std::string& t) {
    auto has = [&](const char* s) { return t.find(s) != std::string::npos; };
    return has("Mineral") || has("Geyser") || has("Vespene");
}

}
