#pragma once

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace omc::formats {

inline constexpr std::size_t kDocumentHeaderDependencyCountOffset = 44;
inline constexpr std::uint32_t kDocumentHeaderMaxDependencies = 256;

inline std::vector<std::string> parse_document_dependencies(const std::vector<std::uint8_t>& b) {
    std::vector<std::string> out;
    if (b.size() < kDocumentHeaderDependencyCountOffset + 4) return out;
    if (std::memcmp(b.data(), "H2CS", 4) != 0) return out;
    std::uint32_t count = 0;
    std::memcpy(&count, b.data() + kDocumentHeaderDependencyCountOffset, 4);
    if (count > kDocumentHeaderMaxDependencies) return out;
    std::size_t o = kDocumentHeaderDependencyCountOffset + 4;
    for (std::uint32_t i = 0; i < count; ++i) {
        std::size_t e = o;
        while (e < b.size() && b[e] != 0) ++e;
        if (e >= b.size()) return {};
        const std::string entry(reinterpret_cast<const char*>(b.data() + o), e - o);
        o = e + 1;
        const std::size_t f = entry.find("file:");
        std::string path = f == std::string::npos ? entry : entry.substr(f + 5);
        const std::size_t comma = path.find(',');
        if (comma != std::string::npos) path.resize(comma);
        if (!path.empty()) out.push_back(std::move(path));
    }
    return out;
}

inline std::string normalize_dependency_path(const std::string& path) {
    std::string key = path;
    for (char& c : key) c = c == '\\' ? '/' : char(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
    while (!key.empty() && key.back() == '/') key.pop_back();
    return key;
}

}
