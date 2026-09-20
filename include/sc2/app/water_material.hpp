#pragma once
#include <cstdlib>
#include <string>
#include <unordered_map>

#include "sc2/casc/storage.hpp"

namespace sc2::app {

struct WaterMaterial {
    bool has = false;
    float height = 0.0f;
    float color[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    float color_falloff = 0.0f;
    float specularity = 0.0f;
    float specular_scaler = 0.0f;
    float reflectivity_power = 0.0f;
    float min_reflection = 0.0f;
    float reflection_distortion = 0.0f;
    float refraction_distortion = 0.0f;
    float texture_roughness = 0.0f;
    float mesh_roughness = 0.0f;
    float uv_rate[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    float uv_rotate = 0.0f;
};

namespace detail {
inline bool water_field(const std::string& xml, std::size_t from, std::size_t to, const char* field, float* out, int n) {
    const std::string key = std::string("<") + field + " value=\"";
    std::size_t p = xml.find(key, from);
    if (p == std::string::npos || p >= to) return false;
    p += key.size();
    for (int i = 0; i < n; ++i) {
        char* end = nullptr;
        out[i] = std::strtof(xml.c_str() + p, &end);
        if (!end || end == xml.c_str() + p) return i > 0;
        p = std::size_t(end - xml.c_str());
        if (p < xml.size() && xml[p] == ',') ++p;
    }
    return true;
}
}

inline std::unordered_map<std::string, WaterMaterial> water_type_materials(casc::Storage& store) {
    std::unordered_map<std::string, WaterMaterial> out;
    static const char* kMods[] = {"void", "swarm", "liberty", "core"};
    for (const char* m : kMods) {
        std::string xml;
        try {
            auto b = store.read_by_name(std::string("mods/") + m + ".sc2mod/base.sc2data/GameData/WaterData.xml");
            xml.assign(b.begin(), b.end());
        } catch (const std::exception&) {
            continue;
        }
        std::size_t pos = 0;
        while (true) {
            std::size_t tag = xml.find("<CWater id=\"", pos);
            if (tag == std::string::npos) break;
            std::size_t idStart = tag + 12;
            std::size_t idEnd = xml.find('"', idStart);
            if (idEnd == std::string::npos) break;
            const std::string id = xml.substr(idStart, idEnd - idStart);
            pos = idEnd + 1;
            if (out.count(id)) continue;
            std::size_t gt = xml.find_first_of("/>", pos);
            if (gt == std::string::npos) break;
            if (xml[gt] == '/') { pos = gt + 2; continue; }
            pos = gt + 1;
            std::size_t blockEnd = xml.find("</CWater>", pos);
            if (blockEnd == std::string::npos) break;
            std::size_t stateStart = xml.find("<State", pos);
            if (stateStart != std::string::npos && stateStart < blockEnd) {
                std::size_t nextState = xml.find("<State", stateStart + 1);
                const std::size_t stateEnd = (nextState != std::string::npos && nextState < blockEnd) ? nextState : blockEnd;
                WaterMaterial w;
                float one[1];
                if (detail::water_field(xml, stateStart, stateEnd, "Height", one, 1)) w.height = one[0];
                w.has = detail::water_field(xml, stateStart, stateEnd, "Color", w.color, 4);
                if (detail::water_field(xml, stateStart, stateEnd, "ColorFallOff", one, 1)) w.color_falloff = one[0];
                if (detail::water_field(xml, stateStart, stateEnd, "Specularity", one, 1)) w.specularity = one[0];
                if (detail::water_field(xml, stateStart, stateEnd, "SpecularScaler", one, 1)) w.specular_scaler = one[0];
                if (detail::water_field(xml, stateStart, stateEnd, "ReflectivityPower", one, 1)) w.reflectivity_power = one[0];
                if (detail::water_field(xml, stateStart, stateEnd, "MinReflection", one, 1)) w.min_reflection = one[0];
                if (detail::water_field(xml, stateStart, stateEnd, "ReflectionDistortion", one, 1)) w.reflection_distortion = one[0];
                if (detail::water_field(xml, stateStart, stateEnd, "RefractionDistortion", one, 1)) w.refraction_distortion = one[0];
                if (detail::water_field(xml, stateStart, stateEnd, "TextureRoughness", one, 1)) w.texture_roughness = one[0];
                if (detail::water_field(xml, stateStart, stateEnd, "MeshRoughness", one, 1)) w.mesh_roughness = one[0];
                detail::water_field(xml, stateStart, stateEnd, "UvRate", w.uv_rate, 4);
                if (detail::water_field(xml, stateStart, stateEnd, "UvRotate", one, 1)) w.uv_rotate = one[0];
                if (w.has) out.emplace(id, w);
            }
            pos = blockEnd + 9;
        }
    }
    return out;
}

}
