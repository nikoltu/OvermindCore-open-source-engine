#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace sc2::core {

struct XmlNode {
    std::string name;
    std::vector<std::pair<std::string, std::string>> attrs;
    std::vector<XmlNode> children;
    std::string text;

    const std::string* attr(const std::string& key) const {
        for (const auto& [k, v] : attrs)
            if (k == key) return &v;
        return nullptr;
    }
    const std::string& attr_or(const std::string& key,
                               const std::string& fallback) const {
        const std::string* v = attr(key);
        return v ? *v : fallback;
    }
    std::vector<const XmlNode*> elements(const std::string& tag) const {
        std::vector<const XmlNode*> out;
        for (const auto& c : children)
            if (c.name == tag) out.push_back(&c);
        return out;
    }
};

XmlNode xml_parse(const std::string& data);
XmlNode xml_parse(const std::vector<uint8_t>& data);

}
