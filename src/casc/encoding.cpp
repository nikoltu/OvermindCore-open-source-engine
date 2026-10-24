#include "sc2/casc/encoding.hpp"

#include <algorithm>
#include <stdexcept>
#include <string>

namespace sc2::casc {

static uint32_t rd_u32be(const uint8_t* p) {
    return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 |
           uint32_t(p[3]);
}
static uint16_t rd_u16be(const uint8_t* p) {
    return uint16_t(uint16_t(p[0]) << 8 | p[1]);
}

void Encoding::parse(const std::vector<uint8_t>& blob) {
    map_.clear();
    sorted_.clear();
    const bool old = casc_index_maps_old();
    const uint8_t* b = blob.data();
    const size_t n = blob.size();
    if (n < 22 || b[0] != 'E' || b[1] != 'N')
        throw std::runtime_error("encoding: bad magic");

    uint16_t ckey_page_kb = rd_u16be(b + 5);
    uint32_t ckey_page_count = rd_u32be(b + 9);
    uint32_t espec = rd_u32be(b + 18);

    const size_t page = size_t(ckey_page_kb) * 1024;
    const size_t pages_off = 22 + size_t(espec) + size_t(ckey_page_count) * 32;
    if (page == 0 || pages_off + size_t(ckey_page_count) * page > n)
        throw std::runtime_error("encoding: table out of range");

    if (old) map_.reserve(size_t(ckey_page_count) * 8);
    else sorted_.reserve(size_t(ckey_page_count) * 8);
    for (uint32_t pg = 0; pg < ckey_page_count; ++pg) {
        size_t p = pages_off + size_t(pg) * page;
        const size_t end = p + page;
        while (p + 22 <= end) {
            uint8_t kc = b[p];
            if (kc == 0) break;
            if (p + 22 + size_t(kc) * 16 > end)
                throw std::runtime_error("encoding: entry crosses page");
            CKey ck;
            for (int i = 0; i < 16; ++i) ck[i] = b[p + 6 + i];
            EKey ek;
            for (int i = 0; i < 16; ++i) ek[i] = b[p + 22 + i];
            if (old) map_.emplace(ck, ek);
            else sorted_.emplace_back(ck, ek);
            p += 22 + size_t(kc) * 16;
        }
    }
    if (!old) {
        std::stable_sort(sorted_.begin(), sorted_.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
        sorted_.erase(std::unique(sorted_.begin(), sorted_.end(), [](const auto& a, const auto& b) { return a.first == b.first; }), sorted_.end());
        sorted_.shrink_to_fit();
    }
}

const EKey* Encoding::find(const CKey& ckey) const {
    if (!map_.empty()) {
        auto it = map_.find(ckey);
        return it == map_.end() ? nullptr : &it->second;
    }
    auto it = std::lower_bound(sorted_.begin(), sorted_.end(), ckey, [](const auto& e, const CKey& k) { return e.first < k; });
    return it != sorted_.end() && it->first == ckey ? &it->second : nullptr;
}

CKey Encoding::key_from_hex(const std::string& hex) {
    if (hex.size() < 32) throw std::runtime_error("ckey hex too short");
    auto nib = [](char c) -> uint8_t {
        if (c >= '0' && c <= '9') return uint8_t(c - '0');
        if (c >= 'a' && c <= 'f') return uint8_t(c - 'a' + 10);
        if (c >= 'A' && c <= 'F') return uint8_t(c - 'A' + 10);
        throw std::runtime_error("bad hex");
    };
    CKey k{};
    for (int i = 0; i < 16; ++i)
        k[i] = uint8_t(nib(hex[2 * i]) << 4 | nib(hex[2 * i + 1]));
    return k;
}

}
