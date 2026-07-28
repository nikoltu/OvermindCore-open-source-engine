#include "sc2/formats/mpq.hpp"

#include <algorithm>
#include <array>
#include <stdexcept>
#include <string>
#include <utility>

#include "sc2/core/byte_reader.hpp"
#include "sc2/core/inflate.hpp"

namespace sc2::formats {

using core::ByteReader;

namespace {

using CryptTable = std::array<uint32_t, 0x500>;

const CryptTable& crypt_table() {
    static const CryptTable table = [] {
        CryptTable t{};
        uint32_t seed = 0x00100001u;
        for (uint32_t i = 0; i < 0x100; ++i) {
            uint32_t idx = i;
            for (int k = 0; k < 5; ++k, idx += 0x100) {
                seed = (seed * 125 + 3) % 0x2AAAABu;
                uint32_t a = (seed & 0xFFFFu) << 16;
                seed = (seed * 125 + 3) % 0x2AAAABu;
                uint32_t b = seed & 0xFFFFu;
                t[idx] = a | b;
            }
        }
        return t;
    }();
    return table;
}

enum HashKind : uint32_t {
    kHashOffset = 0x000,
    kHashNameA = 0x100,
    kHashNameB = 0x200,
    kHashKey = 0x300,
};

uint32_t hash_string(const std::string& s, uint32_t kind) {
    const CryptTable& t = crypt_table();
    uint32_t s1 = 0x7FED7FEDu;
    uint32_t s2 = 0xEEEEEEEEu;
    for (char raw : s) {
        uint32_t c = uint8_t(raw);
        if (c >= 'a' && c <= 'z') c -= 32;
        if (c == '/') c = '\\';
        s1 = t[kind + c] ^ (s1 + s2);
        s2 = c + s1 + s2 + (s2 << 5) + 3;
    }
    return s1;
}

void decrypt_block(uint32_t* words, size_t n, uint32_t key) {
    const CryptTable& t = crypt_table();
    uint32_t k = key;
    uint32_t s2 = 0xEEEEEEEEu;
    for (size_t i = 0; i < n; ++i) {
        s2 += t[0x400 + (k & 0xFF)];
        uint32_t v = words[i] ^ (k + s2);
        words[i] = v;
        k = ((~k << 21) + 0x11111111u) | (k >> 11);
        s2 = v + s2 + (s2 << 5) + 3;
    }
}

std::vector<uint32_t> read_u32s(const std::vector<uint8_t>& image, size_t off,
                                size_t count) {
    ByteReader r(image);
    r.seek(off);
    std::vector<uint32_t> out(count);
    for (size_t i = 0; i < count; ++i) out[i] = r.u32le();
    return out;
}

constexpr uint32_t kFlagImplode = 0x00000100u;
constexpr uint32_t kFlagCompress = 0x00000200u;
constexpr uint32_t kFlagEncrypted = 0x00010000u;
constexpr uint32_t kFlagFixKey = 0x00020000u;
constexpr uint32_t kFlagSingleUnit = 0x01000000u;
constexpr uint32_t kFlagExists = 0x80000000u;

constexpr uint32_t kHashEmpty = 0xFFFFFFFFu;
constexpr uint32_t kHashDeleted = 0xFFFFFFFEu;

std::string basename_of(const std::string& name) {
    size_t p = name.find_last_of("\\/");
    return p == std::string::npos ? name : name.substr(p + 1);
}

}

Mpq Mpq::open(const std::vector<uint8_t>& bytes) {
    Mpq m;
    m.image_ = bytes;
    m.parse();
    return m;
}

Mpq Mpq::open(std::vector<uint8_t>&& bytes) {
    Mpq m;
    m.image_ = std::move(bytes);
    m.parse();
    return m;
}

void Mpq::parse() {
    bool found = false;
    for (size_t o = 0; o + 4 <= image_.size(); o += 512) {
        if (image_[o] == 'M' && image_[o + 1] == 'P' && image_[o + 2] == 'Q' &&
            image_[o + 3] == 0x1A) {
            base_ = o;
            found = true;
            break;
        }
    }
    if (!found) throw std::runtime_error("mpq: no MPQ header signature");

    ByteReader h(image_);
    h.seek(base_ + 12);
    format_version_ = h.u16le();
    uint32_t sector_shift = h.u16le();
    if ((sector_shift & 0x1F) > 22)
        throw std::runtime_error("mpq: implausible sector size shift");
    sector_size_ = 512u << (sector_shift & 0x1F);
    uint32_t hash_pos = h.u32le();
    uint32_t block_pos = h.u32le();
    uint32_t hash_count = h.u32le();
    uint32_t block_count = h.u32le();

    if (hash_count) {
        auto words = read_u32s(image_, base_ + hash_pos, size_t(hash_count) * 4);
        decrypt_block(words.data(), words.size(), hash_string("(hash table)", kHashKey));
        hash_.resize(hash_count);
        for (uint32_t i = 0; i < hash_count; ++i) {
            hash_[i].name1 = words[i * 4 + 0];
            hash_[i].name2 = words[i * 4 + 1];
            hash_[i].locale_platform = words[i * 4 + 2];
            hash_[i].block_index = words[i * 4 + 3];
        }
    }

    if (block_count) {
        auto words = read_u32s(image_, base_ + block_pos, size_t(block_count) * 4);
        decrypt_block(words.data(), words.size(), hash_string("(block table)", kHashKey));
        block_.resize(block_count);
        for (uint32_t i = 0; i < block_count; ++i) {
            block_[i].file_pos = words[i * 4 + 0];
            block_[i].comp_size = words[i * 4 + 1];
            block_[i].file_size = words[i * 4 + 2];
            block_[i].flags = words[i * 4 + 3];
        }
    }
}

long Mpq::find(const std::string& name) const {
    if (hash_.empty()) return -1;
    uint32_t size = uint32_t(hash_.size());
    uint32_t start = hash_string(name, kHashOffset) % size;
    uint32_t a = hash_string(name, kHashNameA);
    uint32_t b = hash_string(name, kHashNameB);
    for (uint32_t i = 0; i < size; ++i) {
        const HashEntry& e = hash_[(start + i) % size];
        if (e.block_index == kHashEmpty) return -1;
        if (e.block_index != kHashDeleted && e.name1 == a && e.name2 == b)
            return long(e.block_index);
    }
    return -1;
}

bool Mpq::contains(const std::string& name) const {
    long bi = find(name);
    if (bi < 0 || size_t(bi) >= block_.size()) return false;
    return (block_[size_t(bi)].flags & kFlagExists) != 0;
}

size_t Mpq::file_count() const {
    size_t n = 0;
    for (const HashEntry& e : hash_) {
        if (e.block_index >= block_.size()) continue;
        if (block_[e.block_index].flags & kFlagExists) ++n;
    }
    return n;
}

std::vector<uint8_t> Mpq::read_file(const std::string& name) const {
    long bi = find(name);
    if (bi < 0 || size_t(bi) >= block_.size())
        throw std::runtime_error("mpq: file not found: " + name);
    const BlockEntry& b = block_[size_t(bi)];
    if (!(b.flags & kFlagExists))
        throw std::runtime_error("mpq: no EXISTS flag: " + name);

    const size_t pos = base_ + b.file_pos;
    const uint32_t fsize = b.file_size;
    const bool encrypted = (b.flags & kFlagEncrypted) != 0;
    const bool compressed = (b.flags & (kFlagCompress | kFlagImplode)) != 0;

    uint32_t key = 0;
    if (encrypted) {
        key = hash_string(basename_of(name), kHashKey);
        if (b.flags & kFlagFixKey) key = (key + b.file_pos) ^ fsize;
    }

    auto decrypt_chunk = [&](size_t p, size_t len, uint32_t k) {
        std::vector<uint8_t> buf(image_.data() + p, image_.data() + p + len);
        if (!encrypted) return buf;
        size_t nwords = len / 4;
        if (nwords) {
            std::vector<uint32_t> w(nwords);
            for (size_t i = 0; i < nwords; ++i)
                w[i] = uint32_t(buf[i * 4]) | uint32_t(buf[i * 4 + 1]) << 8 |
                       uint32_t(buf[i * 4 + 2]) << 16 | uint32_t(buf[i * 4 + 3]) << 24;
            decrypt_block(w.data(), nwords, k);
            for (size_t i = 0; i < nwords; ++i) {
                buf[i * 4] = uint8_t(w[i]);
                buf[i * 4 + 1] = uint8_t(w[i] >> 8);
                buf[i * 4 + 2] = uint8_t(w[i] >> 16);
                buf[i * 4 + 3] = uint8_t(w[i] >> 24);
            }
        }
        return buf;
    };

    auto decompress = [&](const std::vector<uint8_t>& buf, size_t out_size) -> std::vector<uint8_t> {
        if (!compressed || buf.size() >= out_size) {
            size_t n = std::min(buf.size(), out_size);
            return std::vector<uint8_t>(buf.data(), buf.data() + n);
        }
        if (b.flags & kFlagImplode) {
            throw std::runtime_error("mpq: PKWARE implode not supported (TODO): " + name);
        }
        if (buf.empty()) throw std::runtime_error("mpq: empty compressed sector: " + name);
        uint8_t method = buf[0];
        const uint8_t* body = buf.data() + 1;
        size_t body_len = buf.size() - 1;
        if (method & 0x02) {
            auto d = core::zlib_inflate(body, body_len);
            if (d.size() > out_size) d.resize(out_size);
            return d;
        }
        if (method == 0)
            return std::vector<uint8_t>(body, body + std::min(body_len, out_size));
        throw std::runtime_error("mpq: unsupported compression 0x" +
                                 std::string(1, "0123456789abcdef"[(method >> 4) & 0xF]) +
                                 std::string(1, "0123456789abcdef"[method & 0xF]) + ": " + name);
    };

    auto require = [&](size_t end) {
        if (end > image_.size()) throw std::runtime_error("mpq: read past end: " + name);
    };

    if (b.flags & kFlagSingleUnit) {
        require(pos + b.comp_size);
        return decompress(decrypt_chunk(pos, b.comp_size, key), fsize);
    }

    if (!compressed) {
        require(pos + fsize);
        if (!encrypted)
            return std::vector<uint8_t>(image_.data() + pos, image_.data() + pos + fsize);
        std::vector<uint8_t> out;
        out.reserve(fsize);
        size_t nsec = (size_t(fsize) + sector_size_ - 1) / sector_size_;
        for (size_t i = 0; i < nsec; ++i) {
            size_t off = i * sector_size_;
            size_t len = std::min<size_t>(sector_size_, fsize - off);
            auto dec = decrypt_chunk(pos + off, len, uint32_t(key + i));
            out.insert(out.end(), dec.begin(), dec.end());
        }
        out.resize(fsize);
        return out;
    }

    size_t nsec = (size_t(fsize) + sector_size_ - 1) / sector_size_;
    require(pos + (nsec + 1) * 4);
    auto sot = read_u32s(image_, pos, nsec + 1);
    if (encrypted) decrypt_block(sot.data(), sot.size(), uint32_t(key - 1));

    std::vector<uint8_t> out;
    out.reserve(fsize);
    for (size_t i = 0; i < nsec; ++i) {
        if (sot[i + 1] < sot[i]) throw std::runtime_error("mpq: bad sector table: " + name);
        size_t sec_off = pos + sot[i];
        size_t sec_len = sot[i + 1] - sot[i];
        require(sec_off + sec_len);
        size_t this_size = std::min<size_t>(sector_size_, fsize - i * sector_size_);
        auto dec = decompress(decrypt_chunk(sec_off, sec_len, uint32_t(key + i)), this_size);
        out.insert(out.end(), dec.begin(), dec.end());
    }
    if (out.size() > fsize) out.resize(fsize);
    return out;
}

std::vector<std::string> Mpq::list() const {
    std::vector<std::string> names;
    if (!contains("(listfile)")) return names;
    std::vector<uint8_t> data;
    try {
        data = read_file("(listfile)");
    } catch (const std::exception&) {
        return names;
    }
    std::string cur;
    auto flush = [&] {
        if (!cur.empty()) names.push_back(cur);
        cur.clear();
    };
    for (uint8_t byte : data) {
        char c = char(byte);
        if (c == '\r' || c == '\n' || c == ';')
            flush();
        else
            cur.push_back(c);
    }
    flush();
    return names;
}

}
