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

using BzCrcTable = std::array<uint32_t, 256>;

const BzCrcTable& bz_crc_table() {
    static const BzCrcTable table = [] {
        BzCrcTable t{};
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i << 24;
            for (int k = 0; k < 8; ++k)
                c = (c & 0x80000000u) ? (c << 1) ^ 0x04C11DB7u : (c << 1);
            t[i] = c;
        }
        return t;
    }();
    return table;
}

uint32_t bz_crc(const std::vector<uint8_t>& data) {
    const BzCrcTable& t = bz_crc_table();
    uint32_t c = 0xFFFFFFFFu;
    for (uint8_t byte : data) c = (c << 8) ^ t[((c >> 24) ^ byte) & 0xFFu];
    return ~c;
}

class BzBitReader {
public:
    BzBitReader(const uint8_t* data, size_t size) : data_(data), nbits_(size * 8) {}

    uint32_t bits(int count) {
        uint32_t v = 0;
        for (int i = 0; i < count; ++i) {
            if (pos_ >= nbits_) throw std::runtime_error("mpq: bzip2 stream truncated");
            v = (v << 1) | ((data_[pos_ >> 3] >> (7 - (pos_ & 7))) & 1u);
            ++pos_;
        }
        return v;
    }

private:
    const uint8_t* data_;
    size_t nbits_;
    size_t pos_ = 0;
};

constexpr int kBzMaxCodeLen = 23;

struct BzGroup {
    std::array<int64_t, kBzMaxCodeLen + 2> limit{};
    std::array<int64_t, kBzMaxCodeLen + 2> base{};
    std::vector<int> perm;
    int min_len = 0;
    int max_len = 0;
};

BzGroup bz_build_group(const std::vector<int>& len) {
    BzGroup g;
    const int alpha = int(len.size());
    g.min_len = kBzMaxCodeLen;
    g.max_len = 0;
    for (int l : len) {
        if (l < g.min_len) g.min_len = l;
        if (l > g.max_len) g.max_len = l;
    }
    g.perm.reserve(alpha);
    for (int l = g.min_len; l <= g.max_len; ++l)
        for (int s = 0; s < alpha; ++s)
            if (len[s] == l) g.perm.push_back(s);
    for (int s = 0; s < alpha; ++s) g.base[len[s] + 1] += 1;
    for (int i = 1; i <= kBzMaxCodeLen + 1; ++i) g.base[i] += g.base[i - 1];
    int64_t vec = 0;
    for (int l = g.min_len; l <= g.max_len; ++l) {
        vec += g.base[l + 1] - g.base[l];
        g.limit[l] = vec - 1;
        vec <<= 1;
    }
    for (int l = g.min_len + 1; l <= g.max_len; ++l)
        g.base[l] = ((g.limit[l - 1] + 1) << 1) - g.base[l];
    return g;
}

int bz_decode_symbol(BzBitReader& br, const BzGroup& g) {
    int l = g.min_len;
    int64_t v = int64_t(br.bits(l));
    while (l <= g.max_len && v > g.limit[l]) {
        ++l;
        if (l > g.max_len) break;
        v = (v << 1) | int64_t(br.bits(1));
    }
    if (l > g.max_len) throw std::runtime_error("mpq: bzip2 invalid Huffman code");
    int64_t rank = v - g.base[l];
    if (rank < 0 || rank >= int64_t(g.perm.size()))
        throw std::runtime_error("mpq: bzip2 invalid Huffman code");
    return g.perm[size_t(rank)];
}

std::vector<uint8_t> bzip2_decompress(const uint8_t* data, size_t size) {
    if (size < 4 || data[0] != 'B' || data[1] != 'Z' || data[2] != 'h')
        throw std::runtime_error("mpq: bzip2 bad stream header");
    const int level = data[3] - '0';
    if (level < 1 || level > 9) throw std::runtime_error("mpq: bzip2 bad block-size level");
    const uint32_t block_max = uint32_t(level) * 100000u;

    BzBitReader br(data, size);
    br.bits(32);

    std::vector<uint8_t> out;
    uint32_t combined_crc = 0;
    for (;;) {
        const uint32_t magic_hi = br.bits(24);
        const uint32_t magic_lo = br.bits(24);
        if (magic_hi == 0x177245u && magic_lo == 0x385090u) {
            const uint32_t stored = br.bits(32);
            if (stored != combined_crc)
                throw std::runtime_error("mpq: bzip2 stream CRC mismatch");
            break;
        }
        if (magic_hi != 0x314159u || magic_lo != 0x265359u)
            throw std::runtime_error("mpq: bzip2 bad block magic");

        const uint32_t block_crc = br.bits(32);
        if (br.bits(1))
            throw std::runtime_error("mpq: bzip2 randomised block not supported");
        const uint32_t orig_ptr = br.bits(24);

        bool used[256] = {};
        const uint32_t ranges = br.bits(16);
        for (int i = 0; i < 16; ++i) {
            if (!((ranges >> (15 - i)) & 1u)) continue;
            const uint32_t m = br.bits(16);
            for (int j = 0; j < 16; ++j)
                if ((m >> (15 - j)) & 1u) used[i * 16 + j] = true;
        }
        std::vector<uint8_t> seq_to_unseq;
        for (int i = 0; i < 256; ++i)
            if (used[i]) seq_to_unseq.push_back(uint8_t(i));
        if (seq_to_unseq.empty()) throw std::runtime_error("mpq: bzip2 empty symbol map");
        const int alpha = int(seq_to_unseq.size()) + 2;
        const int eob = alpha - 1;

        const int ngroups = int(br.bits(3));
        if (ngroups < 2 || ngroups > 6) throw std::runtime_error("mpq: bzip2 bad group count");
        const int nselectors = int(br.bits(15));
        if (nselectors < 1) throw std::runtime_error("mpq: bzip2 bad selector count");

        std::vector<uint8_t> selector(static_cast<size_t>(nselectors));
        {
            std::vector<uint8_t> order(static_cast<size_t>(ngroups));
            for (int i = 0; i < ngroups; ++i) order[size_t(i)] = uint8_t(i);
            for (int i = 0; i < nselectors; ++i) {
                int v = 0;
                while (br.bits(1))
                    if (++v >= ngroups) throw std::runtime_error("mpq: bzip2 bad selector");
                const uint8_t sel = order[size_t(v)];
                for (int k = v; k > 0; --k) order[size_t(k)] = order[size_t(k - 1)];
                order[0] = sel;
                selector[size_t(i)] = sel;
            }
        }

        std::vector<BzGroup> groups;
        groups.reserve(size_t(ngroups));
        for (int g = 0; g < ngroups; ++g) {
            std::vector<int> len(static_cast<size_t>(alpha));
            int cur = int(br.bits(5));
            for (int s = 0; s < alpha; ++s) {
                for (;;) {
                    if (cur < 1 || cur > 20)
                        throw std::runtime_error("mpq: bzip2 bad code length");
                    if (!br.bits(1)) break;
                    if (br.bits(1))
                        --cur;
                    else
                        ++cur;
                }
                len[size_t(s)] = cur;
            }
            groups.push_back(bz_build_group(len));
        }

        std::vector<uint32_t> tt;
        tt.reserve(size_t(block_max));
        std::array<uint32_t, 256> counts{};
        std::vector<uint8_t> mtf(seq_to_unseq.size());
        for (size_t i = 0; i < mtf.size(); ++i) mtf[i] = uint8_t(i);

        int group_no = -1;
        int group_pos = 0;
        auto next_symbol = [&] {
            if (group_pos == 0) {
                if (++group_no >= nselectors)
                    throw std::runtime_error("mpq: bzip2 selector list exhausted");
                group_pos = 50;
            }
            --group_pos;
            return bz_decode_symbol(br, groups[selector[size_t(group_no)]]);
        };

        int sym = next_symbol();
        while (sym != eob) {
            if (sym <= 1) {
                uint64_t run = 0;
                uint64_t weight = 1;
                do {
                    run += uint64_t(sym + 1) * weight;
                    if (run > block_max) throw std::runtime_error("mpq: bzip2 run overflow");
                    weight <<= 1;
                    sym = next_symbol();
                } while (sym <= 1);
                const uint8_t byte = seq_to_unseq[mtf[0]];
                if (tt.size() + run > block_max)
                    throw std::runtime_error("mpq: bzip2 block overflow");
                counts[byte] += uint32_t(run);
                tt.insert(tt.end(), size_t(run), uint32_t(byte));
                continue;
            }
            const size_t nn = size_t(sym - 1);
            if (nn >= mtf.size()) throw std::runtime_error("mpq: bzip2 symbol out of range");
            const uint8_t idx = mtf[nn];
            for (size_t k = nn; k > 0; --k) mtf[k] = mtf[k - 1];
            mtf[0] = idx;
            const uint8_t byte = seq_to_unseq[idx];
            if (tt.size() >= block_max) throw std::runtime_error("mpq: bzip2 block overflow");
            counts[byte] += 1;
            tt.push_back(uint32_t(byte));
            sym = next_symbol();
        }

        const uint32_t nblock = uint32_t(tt.size());
        if (nblock == 0 || orig_ptr >= nblock)
            throw std::runtime_error("mpq: bzip2 bad origin pointer");

        std::array<uint32_t, 257> cftab{};
        for (int i = 0; i < 256; ++i) cftab[size_t(i) + 1] = counts[size_t(i)];
        for (int i = 1; i <= 256; ++i) cftab[size_t(i)] += cftab[size_t(i) - 1];
        for (uint32_t i = 0; i < nblock; ++i) {
            const uint8_t ch = uint8_t(tt[i] & 0xFFu);
            tt[cftab[ch]] |= (i << 8);
            cftab[ch] += 1;
        }
        uint32_t tpos = tt[orig_ptr] >> 8;

        std::vector<uint8_t> block;
        block.reserve(size_t(nblock));
        int run = 0;
        int prev = -1;
        for (uint32_t i = 0; i < nblock; ++i) {
            if (tpos >= nblock) throw std::runtime_error("mpq: bzip2 broken BWT chain");
            tpos = tt[tpos];
            const uint8_t byte = uint8_t(tpos & 0xFFu);
            tpos >>= 8;
            if (run == 4) {
                block.insert(block.end(), size_t(byte), uint8_t(prev));
                run = 0;
                prev = -1;
                continue;
            }
            if (int(byte) == prev) {
                ++run;
            } else {
                run = 1;
                prev = int(byte);
            }
            block.push_back(byte);
        }

        if (bz_crc(block) != block_crc)
            throw std::runtime_error("mpq: bzip2 block CRC mismatch");
        combined_crc = ((combined_crc << 1) | (combined_crc >> 31)) ^ block_crc;
        out.insert(out.end(), block.begin(), block.end());
    }
    return out;
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
        if (method == 0x10) {
            auto d = bzip2_decompress(body, body_len);
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
