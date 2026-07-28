#include "sc2/casc/idx.hpp"

#include <stdexcept>

#include "sc2/core/byte_reader.hpp"
#include "sc2/platform/file.hpp"

namespace sc2::casc {

using core::ByteReader;

static uint8_t hex_nibble(char c) {
    if (c >= '0' && c <= '9') return uint8_t(c - '0');
    if (c >= 'a' && c <= 'f') return uint8_t(c - 'a' + 10);
    if (c >= 'A' && c <= 'F') return uint8_t(c - 'A' + 10);
    throw std::runtime_error("bad hex digit");
}

EKey9 ekey9_from_hex(const std::string& hex) {
    if (hex.size() < 18) throw std::runtime_error("ekey hex too short");
    EKey9 k{};
    for (size_t i = 0; i < 9; ++i)
        k[i] = uint8_t(hex_nibble(hex[2 * i]) << 4 | hex_nibble(hex[2 * i + 1]));
    return k;
}

std::vector<std::pair<EKey9, Location>> IdxIndex::parse_idx(
    const std::vector<uint8_t>& blob) {
    ByteReader r(blob);
    if (blob.size() < 0x28) throw std::runtime_error("idx too small");

    r.seek(0x0C);
    uint8_t size_bytes = r.u8();
    uint8_t off_bytes = r.u8();
    uint8_t key_bytes = r.u8();
    uint8_t segment_bits = r.u8();

    r.seek(0x20);
    uint32_t entries_size = r.u32le();

    if (key_bytes < 9 || off_bytes == 0 || off_bytes > 8 || size_bytes == 0 ||
        size_bytes > 8 || segment_bits >= 64)
        throw std::runtime_error("idx: implausible field widths");

    const size_t entry_len = size_t(key_bytes) + off_bytes + size_bytes;
    const size_t count = entry_len ? entries_size / entry_len : 0;
    const uint64_t offset_mask =
        (segment_bits >= 64) ? ~0ull : ((1ull << segment_bits) - 1);

    std::vector<std::pair<EKey9, Location>> out;
    out.reserve(count);
    r.seek(0x28);
    for (size_t i = 0; i < count; ++i) {
        EKey9 key{};
        for (int b = 0; b < 9; ++b) key[b] = r.u8();
        for (int b = 9; b < key_bytes; ++b) r.u8();

        uint64_t packed = r.uint_be(off_bytes);
        uint64_t size = r.uint_le(size_bytes);

        Location loc;
        loc.file_index = uint32_t(packed >> segment_bits);
        loc.offset = packed & offset_mask;
        loc.size = uint32_t(size);
        out.emplace_back(key, loc);
    }
    return out;
}

void IdxIndex::build(const std::string& data_dir) {
    data_dir_ = data_dir;
    map_.clear();
    for (const auto& path : platform::list_dir(data_dir, ".idx")) {
        auto blob = platform::read_file(path);
        for (auto& [key, loc] : parse_idx(blob)) map_[key] = loc;
    }
}

const Location* IdxIndex::find(const EKey9& key) const {
    auto it = map_.find(key);
    return it == map_.end() ? nullptr : &it->second;
}

const Location* IdxIndex::find_hex(const std::string& ekey_hex) const {
    return find(ekey9_from_hex(ekey_hex));
}

}
