#include "omc/casc/mndx.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <random>
#include <stdexcept>
#include <system_error>

namespace omc::casc {
namespace {

constexpr uint32_t kMarSignature = 0x0052414D;
constexpr uint32_t kLastCKeyEntry = 0x80000000u;
constexpr uint32_t kInvalidIndex = 0xFFFFFFFFu;

struct KthBitTable {
    std::array<uint8_t, 0x800> t{};
    KthBitTable() {
        for (int k = 0; k < 8; ++k) {
            for (int b = 0; b < 256; ++b) {
                uint8_t pos = 7;
                int cnt = 0;
                for (int bit = 0; bit < 8; ++bit) {
                    if (b & (1 << bit)) {
                        if (cnt == k) {
                            pos = uint8_t(bit);
                            break;
                        }
                        ++cnt;
                    }
                }
                t[(k << 8) | b] = pos;
            }
        }
    }
};
const KthBitTable g_kth_bit;

inline uint32_t get_set_bits(uint32_t v) {
    v = ((v >> 1) & 0x55555555u) + (v & 0x55555555u);
    v = ((v >> 2) & 0x33333333u) + (v & 0x33333333u);
    v = ((v >> 4) & 0x0F0F0F0Fu) + (v & 0x0F0F0F0Fu);
    return v * 0x01010101u;
}
inline uint32_t popcount32(uint32_t v) { return (get_set_bits(v) >> 24) & 0xFF; }

inline std::string to_lower_latin1(const std::string& s) {
    std::string out = s;
    for (char& ch : out) {
        auto c = uint8_t(ch);
        if ((c >= 'A' && c <= 'Z') || (c >= 0xC0 && c <= 0xDE && c != 0xD7))
            ch = char(c + 0x20);
    }
    return out;
}

class Stream {
public:
    Stream(const uint8_t* buf, size_t size, size_t pos)
        : buf_(buf), size_(size), pos_(pos) {}

    size_t pos() const { return pos_; }

    uint32_t u32() {
        need(4);
        uint32_t v;
        std::memcpy(&v, buf_ + pos_, 4);
        pos_ += 4;
        return v;
    }
    uint64_t u64() {
        need(8);
        uint64_t v;
        std::memcpy(&v, buf_ + pos_, 8);
        pos_ += 8;
        return v;
    }

    uint64_t begin_array(size_t elem_size) {
        uint64_t byte_count = u64();
        if (byte_count > 0xFFFFFFFFu || (byte_count % elem_size) != 0)
            throw std::runtime_error("mndx: bad array byte count");
        need(size_t(byte_count));
        pending_bytes_ = byte_count;
        return byte_count / elem_size;
    }
    const uint8_t* array_data() const { return buf_ + pos_; }
    void end_array() {
        need(pending_bytes_);
        pos_ += pending_bytes_;
        pos_ += size_t((0u - pending_bytes_) & 7u);
    }

private:
    void need(size_t n) const {
        if (pos_ + n > size_) throw std::runtime_error("mndx: read past end");
    }
    const uint8_t* buf_;
    size_t size_;
    size_t pos_;
    uint64_t pending_bytes_ = 0;
};

std::vector<uint32_t> load_dword_array(Stream& s) {
    uint64_t n = s.begin_array(4);
    std::vector<uint32_t> out(size_t(n), 0u);
    if (n) std::memcpy(out.data(), s.array_data(), size_t(n) * 4);
    s.end_array();
    return out;
}

std::vector<uint8_t> load_byte_array(Stream& s) {
    uint64_t n = s.begin_array(1);
    const uint8_t* d = s.array_data();
    std::vector<uint8_t> out(d, d + size_t(n));
    s.end_array();
    return out;
}

struct BitEntryArray {
    std::vector<uint32_t> items;
    uint32_t bits_per_entry = 0;
    uint32_t entry_bit_mask = 0;
    uint64_t total_entries = 0;

    void load(Stream& s) {
        items = load_dword_array(s);
        bits_per_entry = s.u32();
        if (bits_per_entry > 0x20)
            throw std::runtime_error("mndx: bad BitsPerEntry");
        entry_bit_mask = s.u32();
        total_entries = s.u64();
        if (total_entries > 0xFFFFFFFFu)
            throw std::runtime_error("mndx: bad TotalEntries");
    }

    uint32_t get_item(uint32_t entry_index) const {
        uint32_t bit_pos = entry_index * bits_per_entry;
        uint32_t item_index = bit_pos >> 5;
        uint32_t start_bit = bit_pos & 0x1F;
        uint32_t end_bit = start_bit + bits_per_entry;
        uint32_t res;
        if (end_bit > 0x20)
            res = (items.at(item_index + 1) << (0x20 - start_bit)) |
                  (items.at(item_index) >> start_bit);
        else
            res = items.at(item_index) >> start_bit;
        return res & entry_bit_mask;
    }
};

struct Baseval {
    uint32_t v[8];
};

struct SparseArray {
    std::vector<uint32_t> item_bits;
    uint32_t total = 0;
    uint32_t valid = 0;
    std::vector<Baseval> basevals;
    std::vector<uint32_t> idx0;
    std::vector<uint32_t> idx1;

    void load(Stream& s) {
        item_bits = load_dword_array(s);
        total = s.u32();
        valid = s.u32();
        if (valid > total) throw std::runtime_error("mndx: valid > total");
        uint64_t n = s.begin_array(12);
        const uint8_t* d = s.array_data();
        basevals.resize(size_t(n));
        for (size_t i = 0; i < n; ++i) {
            uint32_t base, w0, w1;
            std::memcpy(&base, d + i * 12 + 0, 4);
            std::memcpy(&w0, d + i * 12 + 4, 4);
            std::memcpy(&w1, d + i * 12 + 8, 4);
            Baseval& bv = basevals[i];
            bv.v[0] = base;
            bv.v[1] = w0 & 0x7F;
            bv.v[2] = (w0 >> 7) & 0xFF;
            bv.v[3] = (w0 >> 15) & 0xFF;
            bv.v[4] = (w0 >> 23) & 0x1FF;
            bv.v[5] = w1 & 0x1FF;
            bv.v[6] = (w1 >> 9) & 0x1FF;
            bv.v[7] = (w1 >> 18) & 0x1FF;
        }
        s.end_array();
        idx0 = load_dword_array(s);
        idx1 = load_dword_array(s);
    }

    bool is_empty() const { return total == 0; }

    bool is_item_present(uint32_t index) const {
        return (item_bits.at(index >> 5) & (1u << (index & 0x1F))) != 0;
    }

    uint32_t get_item_value_at(uint32_t index) const {
        const Baseval& bv = basevals.at(index >> 9);
        uint32_t val = bv.v[0];
        uint32_t sel = (index >> 6) & 0x07;
        if (sel) val += bv.v[sel];
        if (index & 0x20) val += popcount32(item_bits.at((index >> 5) - 1));
        uint32_t mask = (1u << (index & 0x1F)) - 1;
        val += popcount32(item_bits.at(index >> 5) & mask);
        return val;
    }

    uint32_t find_group_items0(uint32_t index) const {
        uint32_t g = index >> 9;
        uint32_t min_group = idx0.at(g) >> 9;
        uint32_t max_group = (idx0.at(g + 1) + 0x1FF) >> 9;
        if ((max_group - min_group) < 10) {
            while (index >= (min_group << 9) - basevals.at(min_group + 1).v[0] + 0x200)
                ++min_group;
        } else {
            while ((min_group + 1) < max_group) {
                uint32_t mid = (max_group + min_group) >> 1;
                if (index < (max_group << 9) - basevals.at(max_group).v[0])
                    max_group = mid;
                else
                    min_group = mid;
            }
        }
        return min_group;
    }

    uint32_t find_group_items1(uint32_t index) const {
        uint32_t g = index >> 9;
        uint32_t start = idx1.at(g) >> 9;
        uint32_t nxt = (idx1.at(g + 1) + 0x1FF) >> 9;
        if ((nxt - start) < 10) {
            while (index >= basevals.at(start + 1).v[0]) ++start;
        } else {
            if ((start + 1) < nxt) {
                uint32_t mid = (nxt + start) >> 1;
                if (index < basevals.at(mid).v[0])
                    nxt = mid;
                else
                    start = mid;
            }
        }
        return start;
    }

    uint32_t get_item0(uint32_t index) const {
        if ((index & 0x1FF) == 0) return idx0.at(index >> 9);
        uint32_t gi = find_group_items0(index);
        const Baseval& bv = basevals.at(gi);
        int64_t edx = uint32_t(index + bv.v[0] - (gi << 9));
        uint32_t dword_index = gi << 4;
        if (edx < int64_t(0x100) - bv.v[4]) {
            if (edx < int64_t(0x80) - bv.v[2]) {
                if (edx >= int64_t(0x40) - bv.v[1]) {
                    dword_index += 2;
                    edx = edx + bv.v[1] - 0x40;
                }
            } else {
                if (edx < int64_t(0xC0) - bv.v[3]) {
                    dword_index += 4;
                    edx = edx + bv.v[2] - 0x80;
                } else {
                    dword_index += 6;
                    edx = edx + bv.v[3] - 0xC0;
                }
            }
        } else {
            if (edx < int64_t(0x180) - bv.v[6]) {
                if (edx < int64_t(0x140) - bv.v[5]) {
                    dword_index += 8;
                    edx = edx + bv.v[4] - 0x100;
                } else {
                    dword_index += 10;
                    edx = edx + bv.v[5] - 0x140;
                }
            } else {
                if (edx < int64_t(0x1C0) - bv.v[7]) {
                    dword_index += 12;
                    edx = edx + bv.v[6] - 0x180;
                } else {
                    dword_index += 14;
                    edx = edx + bv.v[7] - 0x1C0;
                }
            }
        }
        uint32_t bit_group = ~item_bits.at(dword_index);
        uint32_t zb = get_set_bits(bit_group);
        uint32_t zb32 = (zb >> 24) & 0xFF;
        if (edx >= int64_t(zb32)) {
            ++dword_index;
            bit_group = ~item_bits.at(dword_index);
            edx -= zb32;
            zb = get_set_bits(bit_group);
        }
        uint32_t item_index = dword_index << 5;
        uint32_t zb08 = zb & 0xFF;
        uint32_t zb16 = (zb >> 8) & 0xFF;
        uint32_t zb24 = (zb >> 16) & 0xFF;
        if (edx < int64_t(zb16)) {
            if (edx >= int64_t(zb08)) {
                bit_group >>= 0x08;
                item_index += 0x08;
                edx -= zb08;
            }
        } else {
            if (edx < int64_t(zb24)) {
                bit_group >>= 0x10;
                item_index += 0x10;
                edx -= zb16;
            } else {
                bit_group >>= 0x18;
                item_index += 0x18;
                edx -= zb24;
            }
        }
        edx <<= 0x08;
        bit_group &= 0xFF;
        return g_kth_bit.t[bit_group + size_t(edx)] + item_index;
    }

    uint32_t get_item1(uint32_t index) const {
        if ((index & 0x1FF) == 0) return idx1.at(index >> 9);
        uint32_t gi = find_group_items1(index);
        const Baseval& bv = basevals.at(gi);
        int64_t dist = uint32_t(index - bv.v[0]);
        uint32_t dword_index = gi << 4;
        if (dist < int64_t(bv.v[4])) {
            if (dist < int64_t(bv.v[2])) {
                if (dist >= int64_t(bv.v[1])) {
                    dist -= bv.v[1];
                    dword_index += 2;
                }
            } else {
                if (dist < int64_t(bv.v[3])) {
                    dist -= bv.v[2];
                    dword_index += 4;
                } else {
                    dist -= bv.v[3];
                    dword_index += 6;
                }
            }
        } else {
            if (dist < int64_t(bv.v[6])) {
                if (dist < int64_t(bv.v[5])) {
                    dist -= bv.v[4];
                    dword_index += 8;
                } else {
                    dist -= bv.v[5];
                    dword_index += 10;
                }
            } else {
                if (dist < int64_t(bv.v[7])) {
                    dist -= bv.v[6];
                    dword_index += 12;
                } else {
                    dist -= bv.v[7];
                    dword_index += 14;
                }
            }
        }
        uint32_t bit_group = item_bits.at(dword_index);
        uint32_t sb = get_set_bits(bit_group);
        uint32_t sb32 = (sb >> 24) & 0xFF;
        if (dist >= int64_t(sb32)) {
            ++dword_index;
            bit_group = item_bits.at(dword_index);
            dist -= sb32;
            sb = get_set_bits(bit_group);
        }
        uint32_t item_index = dword_index << 5;
        uint32_t sb08 = sb & 0xFF;
        uint32_t sb16 = (sb >> 8) & 0xFF;
        uint32_t sb24 = (sb >> 16) & 0xFF;
        if (dist < int64_t(sb16)) {
            if (dist >= int64_t(sb08)) {
                item_index += 0x08;
                bit_group >>= 0x08;
                dist -= sb08;
            }
        } else {
            if (dist < int64_t(sb24)) {
                bit_group >>= 0x10;
                item_index += 0x10;
                dist -= sb16;
            } else {
                bit_group >>= 0x18;
                item_index += 0x18;
                dist -= sb24;
            }
        }
        bit_group &= 0xFF;
        dist <<= 0x08;
        return g_kth_bit.t[bit_group + size_t(dist)] + item_index;
    }
};

struct PathFragmentTable {
    std::vector<uint8_t> fragments;
    SparseArray marks;

    void load(Stream& s) {
        fragments = load_byte_array(s);
        marks.load(s);
    }

    void copy_fragment(std::string& out, uint32_t off) const {
        if (marks.is_empty()) {
            while (off < fragments.size() && fragments[off] != 0) out.push_back(char(fragments[off++]));
        } else {
            while (off < fragments.size() && !marks.is_item_present(off)) out.push_back(char(fragments[off++]));
        }
    }
};

struct HashEntry {
    uint32_t node;
    uint32_t next;
    uint32_t frag;
};

struct FileNameDatabase {
    SparseArray collision;
    SparseArray file_name_indexes;
    SparseArray collision_hibits;
    std::vector<uint8_t> lobits;
    BitEntryArray hibits;
    PathFragmentTable pathfrag;
    std::unique_ptr<FileNameDatabase> child;
    std::vector<HashEntry> hash;
    uint32_t hash_mask = 0;
    uint32_t field_214 = 0;
    uint32_t bitmask = 0;

    void load_from_stream(Stream& s) {
        collision.load(s);
        file_name_indexes.load(s);
        collision_hibits.load(s);
        lobits = load_byte_array(s);
        hibits.load(s);
        pathfrag.load(s);

        if (collision_hibits.valid != 0 && pathfrag.fragments.empty()) {
            child = std::make_unique<FileNameDatabase>();
            child->load_from_stream(s);
        }

        uint64_t n = s.begin_array(12);
        const uint8_t* d = s.array_data();
        hash.resize(size_t(n));
        for (size_t i = 0; i < n; ++i) {
            std::memcpy(&hash[i].node, d + i * 12 + 0, 4);
            std::memcpy(&hash[i].next, d + i * 12 + 4, 4);
            std::memcpy(&hash[i].frag, d + i * 12 + 8, 4);
        }
        s.end_array();
        hash_mask = uint32_t(n) - 1;
        field_214 = s.u32();
        bitmask = s.u32();
    }

    static bool is_single_char(uint32_t frag_val) {
        return (frag_val & 0xFFFFFF00u) == 0xFFFFFF00u;
    }
    bool is_path_fragment_string(uint32_t index) const {
        return collision_hibits.is_item_present(index);
    }
    uint32_t path_fragment_offset1(uint32_t index_lobits) const {
        uint32_t ih = collision_hibits.get_item_value_at(index_lobits);
        return (hibits.get_item(ih) << 8) | lobits[index_lobits];
    }
    uint32_t path_fragment_offset2(uint32_t& hibits_state, uint32_t index_lobits) const {
        if (hibits_state == kInvalidIndex)
            hibits_state = collision_hibits.get_item_value_at(index_lobits);
        else
            hibits_state += 1;
        return (hibits.get_item(hibits_state) << 8) | lobits[index_lobits];
    }

    void copy_fragment_by_index(std::string& out, uint32_t table_index) const {
        for (;;) {
            uint32_t he = table_index & hash_mask;
            if (table_index == hash[he].next) {
                uint32_t frag = hash[he].frag;
                if (!is_single_char(frag)) {
                    if (child)
                        child->copy_fragment_by_index(out, frag);
                    else
                        pathfrag.copy_fragment(out, frag);
                } else {
                    out.push_back(char(frag & 0xFF));
                }
                table_index = hash[he].node;
                if (table_index == 0) return;
            } else {
                if (is_path_fragment_string(table_index)) {
                    uint32_t frag_off = path_fragment_offset1(table_index);
                    if (child)
                        child->copy_fragment_by_index(out, frag_off);
                    else
                        pathfrag.copy_fragment(out, frag_off);
                } else {
                    out.push_back(char(lobits[table_index]));
                }
                if (table_index <= field_214) return;
                table_index = kInvalidIndex - table_index +
                              collision.get_item1(table_index);
            }
        }
    }
};

struct PathStop {
    uint32_t lo_bits_index;
    uint32_t field_4;
    uint32_t count;
    uint32_t hibits_pathfrag = kInvalidIndex;
    uint32_t field_10 = kInvalidIndex;
    PathStop(uint32_t lo, uint32_t f4, uint32_t cnt)
        : lo_bits_index(lo), field_4(f4), count(cnt) {}
};

template <class Cb>
void enumerate_db(const FileNameDatabase& db, Cb&& cb) {
    const SparseArray& coll = db.collision;
    const SparseArray& fni = db.file_name_indexes;
    const std::vector<uint8_t>& lobits = db.lobits;

    std::vector<PathStop> stops;
    std::string path;
    size_t item_count;

    stops.emplace_back(0, 0, 0);
    item_count = 1;
    if (fni.is_item_present(0)) cb(path, fni.get_item_value_at(0));

    for (;;) {
        if (item_count == stops.size()) {
            const PathStop& last = stops[item_count - 1];
            uint32_t col_index = coll.get_item0(last.lo_bits_index) + 1;
            stops.emplace_back(col_index - last.lo_bits_index - 1, col_index, 0);
        }
        PathStop& ps = stops[item_count];
        bool present = coll.is_item_present(ps.field_4);
        ps.field_4 += 1;
        if (present) {
            ++item_count;
            if (db.is_path_fragment_string(ps.lo_bits_index)) {
                uint32_t frag_off =
                    db.path_fragment_offset2(ps.hibits_pathfrag, ps.lo_bits_index);
                if (db.child)
                    db.child->copy_fragment_by_index(path, frag_off);
                else
                    db.pathfrag.copy_fragment(path, frag_off);
            } else {
                path.push_back(char(lobits[ps.lo_bits_index]));
            }
            ps.count = uint32_t(path.size());
            if (fni.is_item_present(ps.lo_bits_index)) {
                if (ps.field_10 == kInvalidIndex)
                    ps.field_10 = fni.get_item_value_at(ps.lo_bits_index);
                else
                    ps.field_10 += 1;
                cb(path, ps.field_10);
            }
        } else {
            if (item_count == 1) return;
            stops[item_count - 1].lo_bits_index += 1;
            uint32_t edi = stops[item_count - 2].count;
            path.resize(edi);
            --item_count;
        }
    }
}

FileNameDatabase load_mar(const uint8_t* buf, size_t size, uint64_t offset,
                          uint64_t mar_size) {
    Stream s(buf, size, size_t(offset));
    (void)mar_size;
    if (s.u32() != kMarSignature)
        throw std::runtime_error("mndx: bad MAR signature");
    FileNameDatabase db;
    db.load_from_stream(s);
    return db;
}

struct MndxHeader {
    uint32_t signature;
    uint32_t ckey_entries_offset;
    uint32_t ckey_entries_count;
    uint32_t file_name_count;
    uint32_t mar_info_offset;
    uint32_t mar_info_count;
    uint32_t mar_info_size;
};

struct MarInfo {
    uint64_t offset;
    uint64_t size;
};

struct CKeyEntry {
    uint32_t flags;
    CKey ckey;
};

}

void Mndx::parse(const std::vector<uint8_t>& root_blob) {
    entries_.clear();
    by_path_.clear();
    by_hash_.clear();
    resolve_ck_.clear();
    fc_.clear();
    restart_.clear();
    first_ck_.clear();
    fc_count_ = 0;

    const uint8_t* buf = root_blob.data();
    const size_t size = root_blob.size();
    if (size < 0x30 || std::memcmp(buf, "MNDX", 4) != 0)
        throw std::runtime_error("mndx: not an MNDX blob");

    auto rd32 = [&](size_t off) {
        if (off > size || size - off < 4) throw std::runtime_error("mndx: header offset past end of blob");
        uint32_t v;
        std::memcpy(&v, buf + off, 4);
        return v;
    };

    MndxHeader hdr{};
    hdr.signature = rd32(0x00);
    hdr.mar_info_offset = rd32(0x14);
    hdr.mar_info_count = rd32(0x18);
    hdr.mar_info_size = rd32(0x1C);
    hdr.ckey_entries_offset = rd32(0x20);
    hdr.ckey_entries_count = rd32(0x24);
    hdr.file_name_count = rd32(0x28);

    if (hdr.mar_info_size < 20 || hdr.mar_info_count > size / hdr.mar_info_size ||
        hdr.mar_info_offset > size - size_t(hdr.mar_info_count) * hdr.mar_info_size)
        throw std::runtime_error("mndx: MAR-info table does not fit the blob");
    std::vector<MarInfo> mar(hdr.mar_info_count);
    for (uint32_t i = 0; i < hdr.mar_info_count; ++i) {
        size_t o = hdr.mar_info_offset + size_t(i) * hdr.mar_info_size;
        uint64_t sz = rd32(o + 4) | (uint64_t(rd32(o + 8)) << 32);
        uint64_t of = rd32(o + 12) | (uint64_t(rd32(o + 16)) << 32);
        mar[i] = {of, sz};
    }
    if (mar.size() < 3) throw std::runtime_error("mndx: missing MAR files");

    if (hdr.ckey_entries_count > size / 24 ||
        hdr.ckey_entries_offset > size - size_t(hdr.ckey_entries_count) * 24)
        throw std::runtime_error("mndx: CKey table does not fit the blob");
    std::vector<CKeyEntry> ckeys(hdr.ckey_entries_count);
    for (uint32_t i = 0; i < hdr.ckey_entries_count; ++i) {
        size_t o = hdr.ckey_entries_offset + size_t(i) * 24;
        ckeys[i].flags = rd32(o);
        std::memcpy(ckeys[i].ckey.data(), buf + o + 4, 16);
    }

    std::vector<uint32_t> f2c(size_t(hdr.file_name_count) + 1, 0);
    {
        uint32_t fi = 1;
        for (uint32_t i = 0; i < hdr.ckey_entries_count; ++i) {
            if (fi > hdr.file_name_count) break;
            if (ckeys[i].flags & kLastCKeyEntry) f2c[fi++] = i + 1;
        }
    }

    FileNameDatabase pkg_db = load_mar(buf, size, mar[0].offset, mar[0].size);
    std::unordered_map<uint32_t, std::string> packages;
    enumerate_db(pkg_db, [&](const std::string& name, uint32_t ni) {
        packages.emplace(ni, name);
    });

    FileNameDatabase stripped_db = load_mar(buf, size, mar[1].offset, mar[1].size);
    std::unordered_map<std::string, CKey> lc_to_ckey;
    lc_to_ckey.reserve(hdr.ckey_entries_count * 2);
    const uint32_t n_entries = hdr.ckey_entries_count;
    enumerate_db(stripped_db, [&](const std::string& stripped, uint32_t nindex) {
        if (nindex >= hdr.file_name_count) return;
        uint32_t i = f2c[nindex];
        while (i < n_entries) {
            uint32_t flags = ckeys[i].flags;
            auto it = packages.find(flags & 0x00FFFFFFu);
            if (it != packages.end()) {
                std::string full = it->second;
                full.push_back('/');
                full += stripped;
                lc_to_ckey[to_lower_latin1(full)] = ckeys[i].ckey;
            }
            if (flags & kLastCKeyEntry) break;
            ++i;
        }
    });

    FileNameDatabase full_db = load_mar(buf, size, mar[2].offset, mar[2].size);
    entries_.reserve(hdr.file_name_count);
    by_path_.reserve(hdr.file_name_count * 2);
    static const CKey kZeroKey{};
    enumerate_db(full_db, [&](const std::string& name, uint32_t) {
        auto it = lc_to_ckey.find(to_lower_latin1(name));
        const CKey& ck = (it != lc_to_ckey.end()) ? it->second : kZeroKey;
        auto ins = by_path_.emplace(name, ck);
        if (ins.second)
            entries_.emplace_back(name, ck);
        else
            ins.first->second = ck;
    });
    if (casc_index_maps_old()) return;
    resolve_ck_.resize(entries_.size());
    by_hash_.resize(entries_.size());
    for (std::size_t i = 0; i < entries_.size(); ++i) {
        resolve_ck_[i] = by_path_.find(entries_[i].first)->second;
        by_hash_[i] = {fnv1a64(entries_[i].first), std::uint32_t(i)};
    }
    std::sort(by_hash_.begin(), by_hash_.end());
    std::unordered_map<std::string, CKey>().swap(by_path_);
    static const bool kPlainNames = std::getenv("OMC_CASC_NAMES_PLAIN") != nullptr;
    if (kPlainNames) {
        entries_.shrink_to_fit();
        return;
    }
    const std::size_t n = entries_.size();
    std::size_t blob = 0;
    for (std::size_t i = 0; i < n; ++i) {
        std::size_t shared = 0;
        if (i % kRestart) {
            const std::string& a = entries_[i - 1].first;
            const std::string& s = entries_[i].first;
            const std::size_t lim = std::min(a.size(), s.size());
            while (shared < lim && a[shared] == s[shared]) ++shared;
        }
        blob += 6 + entries_[i].first.size() - shared;
    }
    fc_.reserve(blob);
    restart_.reserve(n / kRestart + 1);
    auto put_varint = [this](std::uint32_t v) {
        while (v >= 0x80) {
            fc_.push_back(std::uint8_t(v | 0x80));
            v >>= 7;
        }
        fc_.push_back(std::uint8_t(v));
    };
    for (std::size_t i = 0; i < n; ++i) {
        const std::string& s = entries_[i].first;
        if (entries_[i].second != resolve_ck_[i]) first_ck_.emplace_back(std::uint32_t(i), entries_[i].second);
        std::size_t shared = 0;
        if (i % kRestart == 0) {
            restart_.push_back(std::uint32_t(fc_.size()));
        } else {
            const std::string& a = entries_[i - 1].first;
            const std::size_t lim = std::min(a.size(), s.size());
            while (shared < lim && a[shared] == s[shared]) ++shared;
        }
        put_varint(std::uint32_t(shared));
        put_varint(std::uint32_t(s.size() - shared));
        fc_.insert(fc_.end(), s.begin() + std::ptrdiff_t(shared), s.end());
    }
    fc_.shrink_to_fit();
    first_ck_.shrink_to_fit();
    fc_count_ = n;
    std::vector<std::pair<std::string, CKey>>().swap(entries_);
}

namespace {
std::uint32_t get_varint(const std::vector<std::uint8_t>& b, std::size_t& p) {
    std::uint32_t v = 0;
    for (int shift = 0; p < b.size(); shift += 7) {
        const std::uint8_t c = b[p++];
        v |= std::uint32_t(c & 0x7f) << shift;
        if (!(c & 0x80)) break;
    }
    return v;
}

void fc_step(const std::vector<std::uint8_t>& b, std::size_t& p, std::string& s) {
    const std::uint32_t shared = get_varint(b, p);
    const std::uint32_t rest = get_varint(b, p);
    s.resize(std::min<std::size_t>(shared, s.size()));
    const std::size_t take = std::min<std::size_t>(rest, b.size() - std::min(p, b.size()));
    s.append(reinterpret_cast<const char*>(b.data() + p), take);
    p += take;
}
}

namespace {
constexpr char kCacheMagic[8] = {'O', 'M', 'C', 'M', 'N', 'D', 'X', '1'};

std::uint64_t cache_sum(const std::uint8_t* p, std::size_t n) {
    std::uint64_t h = 1469598103934665603ull;
    const std::size_t words = n / 8;
    for (std::size_t i = 0; i < words; ++i) {
        std::uint64_t w;
        std::memcpy(&w, p + i * 8, 8);
        h = (h ^ w) * 1099511628211ull;
    }
    for (std::size_t i = words * 8; i < n; ++i) h = (h ^ p[i]) * 1099511628211ull;
    return h;
}

template <class T>
void cache_put(std::vector<std::uint8_t>& out, const std::vector<T>& v) {
    const std::uint64_t n = v.size();
    const std::size_t at = out.size();
    out.resize(at + 8 + n * sizeof(T));
    std::memcpy(out.data() + at, &n, 8);
    if (n) std::memcpy(out.data() + at + 8, v.data(), n * sizeof(T));
}

template <class T>
bool cache_get(const std::vector<std::uint8_t>& in, std::size_t& at, std::size_t end, std::vector<T>& v) {
    std::uint64_t n = 0;
    if (end - at < 8) return false;
    std::memcpy(&n, in.data() + at, 8);
    at += 8;
    if (n > (end - at) / sizeof(T)) return false;
    v.resize(std::size_t(n));
    if (n) std::memcpy(static_cast<void*>(v.data()), in.data() + at, std::size_t(n) * sizeof(T));
    at += std::size_t(n) * sizeof(T);
    return true;
}
}

bool Mndx::save_cache(const std::string& path, const std::string& key) const {
    if (!fc_count_) return false;
    std::vector<std::uint8_t> out(kCacheMagic, kCacheMagic + 8);
    const std::vector<char> key_bytes(key.begin(), key.end());
    cache_put(out, key_bytes);
    const std::vector<std::uint64_t> count{std::uint64_t(fc_count_)};
    cache_put(out, count);
    cache_put(out, fc_);
    cache_put(out, restart_);
    cache_put(out, first_ck_);
    cache_put(out, by_hash_);
    cache_put(out, resolve_ck_);
    const std::uint64_t sum = cache_sum(out.data(), out.size());
    const std::size_t at = out.size();
    out.resize(at + 8);
    std::memcpy(out.data() + at, &sum, 8);
    static std::atomic<std::uint32_t> serial{0};
    const std::string tmp = path + "." + std::to_string(std::random_device{}()) + "." + std::to_string(serial.fetch_add(1)) + ".tmp";
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(path).parent_path(), ec);
    {
        std::ofstream f(tmp, std::ios::binary);
        if (!f) return false;
        f.write(reinterpret_cast<const char*>(out.data()), std::streamsize(out.size()));
        if (!f) {
            f.close();
            std::filesystem::remove(tmp, ec);
            return false;
        }
    }
    std::filesystem::rename(tmp, path, ec);
    if (ec) {
        std::error_code ignored;
        std::filesystem::remove(tmp, ignored);
        return false;
    }
    return true;
}

bool Mndx::load_cache(const std::string& path, const std::string& key) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) return false;
    const std::streamoff size = f.tellg();
    if (size < 8 + 8) return false;
    std::vector<std::uint8_t> in{};
    in.resize(std::size_t(size));
    f.seekg(0);
    f.read(reinterpret_cast<char*>(in.data()), size);
    if (!f) return false;
    const std::size_t end = in.size() - 8;
    if (std::memcmp(in.data(), kCacheMagic, 8) != 0) return false;
    std::uint64_t sum = 0;
    std::memcpy(&sum, in.data() + end, 8);
    if (sum != cache_sum(in.data(), end)) return false;
    std::size_t at = 8;
    std::vector<char> key_bytes;
    std::vector<std::uint64_t> count;
    std::vector<std::uint8_t> fc;
    std::vector<std::uint32_t> restart;
    std::vector<std::pair<std::uint32_t, CKey>> first_ck;
    std::vector<std::pair<std::uint64_t, std::uint32_t>> by_hash;
    std::vector<CKey> resolve_ck;
    if (!cache_get(in, at, end, key_bytes) || std::string(key_bytes.begin(), key_bytes.end()) != key) return false;
    if (!cache_get(in, at, end, count) || count.size() != 1 || !count[0]) return false;
    if (!cache_get(in, at, end, fc) || !cache_get(in, at, end, restart) || !cache_get(in, at, end, first_ck) ||
        !cache_get(in, at, end, by_hash) || !cache_get(in, at, end, resolve_ck))
        return false;
    if (at != end || by_hash.size() != count[0] || resolve_ck.size() != count[0]) return false;
    entries_.clear();
    by_path_.clear();
    fc_ = std::move(fc);
    restart_ = std::move(restart);
    first_ck_ = std::move(first_ck);
    by_hash_ = std::move(by_hash);
    resolve_ck_ = std::move(resolve_ck);
    fc_count_ = std::size_t(count[0]);
    return true;
}

std::string Mndx::name(std::size_t i) const {
    if (!fc_count_) return i < entries_.size() ? entries_[i].first : std::string();
    if (i >= fc_count_) return {};
    std::size_t p = restart_[i / kRestart];
    std::string s;
    for (std::size_t k = (i / kRestart) * kRestart; k <= i; ++k) fc_step(fc_, p, s);
    return s;
}

const CKey& Mndx::key(std::size_t i) const {
    if (!fc_count_) return entries_[i].second;
    auto it = std::lower_bound(first_ck_.begin(), first_ck_.end(), std::uint32_t(i),
                               [](const std::pair<std::uint32_t, CKey>& e, std::uint32_t k) { return e.first < k; });
    return it != first_ck_.end() && it->first == i ? it->second : resolve_ck_[i];
}

void Mndx::Range::iterator::load() {
    if (i_ >= m_->size()) return;
    if (!m_->fc_count_) {
        cur_ = m_->entries_[i_];
        return;
    }
    if (pos_ == 0 || i_ % kRestart == 0) {
        pos_ = m_->restart_[i_ / kRestart];
        cur_.first.clear();
        for (std::size_t k = (i_ / kRestart) * kRestart; k <= i_; ++k) fc_step(m_->fc_, pos_, cur_.first);
    } else {
        fc_step(m_->fc_, pos_, cur_.first);
    }
    cur_.second = m_->key(i_);
}

const CKey* Mndx::resolve(const std::string& path) const {
    if (!by_hash_.empty() || by_path_.empty()) {
        const std::uint64_t h = fnv1a64(path);
        auto it = std::lower_bound(by_hash_.begin(), by_hash_.end(), std::make_pair(h, std::uint32_t(0)));
        for (; it != by_hash_.end() && it->first == h; ++it)
            if (name(it->second) == path) return &resolve_ck_[it->second];
        return nullptr;
    }
    auto it = by_path_.find(path);
    return it == by_path_.end() ? nullptr : &it->second;
}

std::size_t Mndx::memory_bytes() const {
    std::size_t b = entries_.capacity() * sizeof(entries_[0]) + by_hash_.capacity() * sizeof(by_hash_[0]) +
                    resolve_ck_.capacity() * sizeof(CKey) + fc_.capacity() + restart_.capacity() * sizeof(restart_[0]) +
                    first_ck_.capacity() * sizeof(first_ck_[0]);
    for (const auto& e : entries_) b += e.first.capacity() > 15 ? e.first.capacity() + 1 : 0;
    for (const auto& kv : by_path_) b += 64 + sizeof(kv) + (kv.first.capacity() > 15 ? kv.first.capacity() + 1 : 0);
    return b;
}

bool casc_index_maps_old() {
    static const bool old = std::getenv("OMC_CASC_INDEX_MAPS") != nullptr;
    return old;
}

}
