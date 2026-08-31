#pragma once

#include <algorithm>
#include <cctype>
#include <cstring>
#include <string>
#include <unordered_set>
#include <vector>

#include "sc2/app/platform.hpp"
#include "sc2/casc/storage.hpp"
#include "sc2/formats/sc2map.hpp"

namespace sc2::app {

struct CascMap {
    enum class Kind {
        Campaign,
        BattlenetArchive,
    };
    std::string path;
    std::string name;
    Kind kind;
    std::uint32_t width = 0, height = 0;
    std::string tileset{};
    bool described = false;
};

namespace detail {

inline bool ends_with_ci(const std::string& s, const char* suffix) {
    const std::string suf(suffix);
    if (s.size() < suf.size()) return false;
    for (std::size_t i = 0; i < suf.size(); ++i) {
        const char a = char(std::tolower((unsigned char)s[s.size() - suf.size() + i]));
        const char b = char(std::tolower((unsigned char)suf[i]));
        if (a != b) return false;
    }
    return true;
}

inline std::string map_stem(const std::string& path) {
    std::size_t slash = path.find_last_of("/\\");
    std::string base = slash == std::string::npos ? path : path.substr(slash + 1);
    std::size_t dot = base.find_last_of('.');
    if (dot == std::string::npos || dot == 0) return base;
    return base.substr(0, dot);
}

inline std::string lower(std::string s) {
    for (char& c : s) c = char(std::tolower((unsigned char)c));
    return s;
}

}

inline std::vector<CascMap> discover_casc_maps(const casc::Storage& store) {
    std::vector<CascMap> out;
    if (!store.has_mndx()) return out;
    std::unordered_set<std::string> seen;
    for (const auto& entry : store.mndx().enumerate()) {
        const std::string& p = entry.first;
        CascMap::Kind kind;
        if (detail::ends_with_ci(p, ".sc2map")) {
            kind = CascMap::Kind::Campaign;
        } else if (detail::ends_with_ci(p, ".s2ma")) {
            kind = CascMap::Kind::BattlenetArchive;
        } else {
            continue;
        }
        if (!seen.insert(detail::lower(p)).second) continue;
        out.push_back(CascMap{p, detail::map_stem(p), kind});
    }
    std::sort(out.begin(), out.end(),
              [](const CascMap& a, const CascMap& b) { return a.path < b.path; });
    return out;
}

inline bool fill_map_facts(std::vector<std::uint8_t> bytes, CascMap& map) {
    if (bytes.size() < 32) return false;
    try {
        formats::Sc2Map m = formats::Sc2Map::open(std::move(bytes));
        const formats::MapInfo& mi = m.info();
        std::string tileset = mi.tileset;
        map.width = mi.width;
        map.height = mi.height;
        map.tileset = std::move(tileset);
        map.described = true;
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

inline bool describe_casc_map(const casc::Storage& store, CascMap& map) {
    std::vector<std::uint8_t> bytes;
    try {
        bytes = store.read_by_name(map.path);
    } catch (const std::exception&) {
        return false;
    }
    return fill_map_facts(std::move(bytes), map);
}

inline std::string doc_header_name(const std::vector<std::uint8_t>& header) {
    static const char kKey[] = "DocInfo/Name";
    const std::size_t kl = 12;
    for (std::size_t i = 0; i + 2 + kl + 4 + 2 <= header.size(); ++i) {
        if (header[i] == 0x0c && header[i + 1] == 0x00 &&
            std::memcmp(&header[i + 2], kKey, kl) == 0) {
            std::size_t p = i + 2 + kl + 4;
            const std::uint16_t vl = std::uint16_t(header[p] | (header[p + 1] << 8));
            p += 2;
            if (vl == 0 || vl > 256 || p + vl > header.size()) return {};
            return std::string(reinterpret_cast<const char*>(&header[p]), vl);
        }
    }
    return {};
}

inline std::string read_casc_map_name(const std::vector<std::uint8_t>& archive_bytes) {
    try {
        formats::Mpq a = formats::Mpq::open(archive_bytes);
        if (!a.contains("DocumentHeader")) return {};
        return doc_header_name(a.read_file("DocumentHeader"));
    } catch (const std::exception&) {
        return {};
    }
}

inline std::vector<platform::MapEntry> casc_maps_to_entries(const std::vector<CascMap>& maps) {
    std::vector<platform::MapEntry> out;
    out.reserve(maps.size());
    for (const CascMap& m : maps) {
        std::string label = m.name;
        if (m.described && m.width > 0 && m.height > 0)
            label += " (" + std::to_string(m.width) + "x" + std::to_string(m.height) + ")";
        out.push_back(platform::MapEntry{m.path, std::move(label)});
    }
    return out;
}

}
