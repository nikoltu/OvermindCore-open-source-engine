#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace omc::casc {

using EKey9 = std::array<uint8_t, 9>;

struct Location {
    uint32_t file_index;
    uint64_t offset;
    uint32_t size;
};

struct EKey9Hash {
    size_t operator()(const EKey9& k) const noexcept {
        size_t h = 1469598103934665603ull;
        for (uint8_t b : k) {
            h ^= b;
            h *= 1099511628211ull;
        }
        return h;
    }
};

class IdxIndex {
public:
    void build(const std::string& data_dir);

    const Location* find(const EKey9& key) const;
    const Location* find_hex(const std::string& ekey_hex) const;

    size_t size() const { return map_.empty() ? sorted_.size() : map_.size(); }
    size_t memory_bytes() const { return sorted_.capacity() * sizeof(sorted_[0]) + map_.size() * (sizeof(EKey9) + sizeof(Location) + 24); }
    const std::string& data_dir() const { return data_dir_; }

    static std::vector<std::pair<EKey9, Location>> parse_idx(
        const std::vector<uint8_t>& blob);

private:
    std::string data_dir_;
    std::unordered_map<EKey9, Location, EKey9Hash> map_;
    std::vector<std::pair<EKey9, Location>> sorted_;
};

EKey9 ekey9_from_hex(const std::string& hex);

}
