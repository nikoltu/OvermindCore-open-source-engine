#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace sc2::casc {

using CKey = std::array<uint8_t, 16>;
using EKey = std::array<uint8_t, 16>;

struct Md5Hash {
    size_t operator()(const std::array<uint8_t, 16>& k) const noexcept {
        size_t h = 1469598103934665603ull;
        for (uint8_t b : k) {
            h ^= b;
            h *= 1099511628211ull;
        }
        return h;
    }
};

class Encoding {
public:
    void parse(const std::vector<uint8_t>& blob);

    const EKey* find(const CKey& ckey) const;
    size_t size() const { return map_.size(); }

    static CKey key_from_hex(const std::string& hex);

private:
    std::unordered_map<CKey, EKey, Md5Hash> map_;
};

}
