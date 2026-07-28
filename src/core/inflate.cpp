#include "sc2/core/inflate.hpp"

#include <stdexcept>

namespace sc2::core {
namespace {

class BitReader {
public:
    BitReader(const uint8_t* d, size_t n) : d_(d), n_(n) {}

    uint32_t bits(int count) {
        uint32_t v = 0;
        for (int i = 0; i < count; ++i) v |= uint32_t(bit()) << i;
        return v;
    }
    int bit() {
        if (bitpos_ == 0) {
            if (bytepos_ >= n_) throw std::runtime_error("inflate: out of input");
            cur_ = d_[bytepos_++];
        }
        int b = (cur_ >> bitpos_) & 1;
        bitpos_ = (bitpos_ + 1) & 7;
        return b;
    }
    void align() { bitpos_ = 0; }
    uint8_t byte() {
        if (bytepos_ >= n_) throw std::runtime_error("inflate: out of input");
        return d_[bytepos_++];
    }
    size_t byte_pos() const { return bytepos_; }

private:
    const uint8_t* d_;
    size_t n_;
    size_t bytepos_ = 0;
    int bitpos_ = 0;
    uint8_t cur_ = 0;
};

struct Huffman {
    std::vector<int> count;
    std::vector<int> symbol;

    void build(const std::vector<int>& lengths) {
        int maxbits = 0;
        for (int l : lengths) maxbits = l > maxbits ? l : maxbits;
        count.assign(maxbits + 1, 0);
        for (int l : lengths)
            if (l) count[l]++;
        symbol.assign(lengths.size(), 0);
        std::vector<int> offsets(maxbits + 2, 0);
        for (int len = 1; len <= maxbits; ++len)
            offsets[len + 1] = offsets[len] + count[len];
        for (size_t sym = 0; sym < lengths.size(); ++sym)
            if (lengths[sym]) symbol[offsets[lengths[sym]]++] = int(sym);
    }

    int decode(BitReader& br) const {
        int code = 0, first = 0, index = 0;
        for (size_t len = 1; len < count.size(); ++len) {
            code |= br.bit();
            int cnt = count[len];
            if (code - first < cnt) return symbol[index + (code - first)];
            index += cnt;
            first += cnt;
            first <<= 1;
            code <<= 1;
        }
        throw std::runtime_error("inflate: bad Huffman code");
    }
};

const uint16_t kLenBase[29] = {3,   4,   5,   6,   7,   8,   9,   10,  11, 13,
                               15,  17,  19,  23,  27,  31,  35,  43,  51, 59,
                               67,  83,  99,  115, 131, 163, 195, 227, 258};
const uint8_t kLenExtra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
                               2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
const uint16_t kDistBase[30] = {1,    2,    3,    4,    5,    7,     9,    13,
                                17,   25,   33,   49,   65,   97,    129,  193,
                                257,  385,  513,  769,  1025, 1537,  2049, 3073,
                                4097, 6145, 8193, 12289, 16385, 24577};
const uint8_t kDistExtra[30] = {0, 0, 0, 0, 1, 1, 2, 2,  3,  3,
                                4, 4, 5, 5, 6, 6, 7, 7,  8,  8,
                                9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

void inflate_block_body(BitReader& br, const Huffman& lit, const Huffman& dist,
                        std::vector<uint8_t>& out) {
    for (;;) {
        int sym = lit.decode(br);
        if (sym == 256) return;
        if (sym < 256) {
            out.push_back(uint8_t(sym));
            continue;
        }
        sym -= 257;
        if (sym >= 29) throw std::runtime_error("inflate: bad length symbol");
        int length = kLenBase[sym] + int(br.bits(kLenExtra[sym]));
        int dsym = dist.decode(br);
        if (dsym >= 30) throw std::runtime_error("inflate: bad distance symbol");
        int distance = kDistBase[dsym] + int(br.bits(kDistExtra[dsym]));
        if (size_t(distance) > out.size())
            throw std::runtime_error("inflate: distance too far back");
        size_t from = out.size() - distance;
        for (int i = 0; i < length; ++i) out.push_back(out[from + i]);
    }
}

Huffman fixed_lit() {
    std::vector<int> l(288);
    for (int i = 0; i < 144; ++i) l[i] = 8;
    for (int i = 144; i < 256; ++i) l[i] = 9;
    for (int i = 256; i < 280; ++i) l[i] = 7;
    for (int i = 280; i < 288; ++i) l[i] = 8;
    Huffman h;
    h.build(l);
    return h;
}
Huffman fixed_dist() {
    std::vector<int> l(30, 5);
    Huffman h;
    h.build(l);
    return h;
}

void read_dynamic(BitReader& br, Huffman& lit, Huffman& dist) {
    static const int kOrder[19] = {16, 17, 18, 0, 8,  7, 9,  6, 10, 5,
                                   11, 4,  12, 3, 13, 2, 14, 1, 15};
    int hlit = int(br.bits(5)) + 257;
    int hdist = int(br.bits(5)) + 1;
    int hclen = int(br.bits(4)) + 4;

    std::vector<int> cl_lengths(19, 0);
    for (int i = 0; i < hclen; ++i) cl_lengths[kOrder[i]] = int(br.bits(3));
    Huffman cl;
    cl.build(cl_lengths);

    std::vector<int> lengths;
    lengths.reserve(hlit + hdist);
    while (int(lengths.size()) < hlit + hdist) {
        int sym = cl.decode(br);
        if (sym < 16) {
            lengths.push_back(sym);
        } else if (sym == 16) {
            if (lengths.empty()) throw std::runtime_error("inflate: repeat w/o prev");
            int rep = 3 + int(br.bits(2));
            int prev = lengths.back();
            while (rep--) lengths.push_back(prev);
        } else if (sym == 17) {
            int rep = 3 + int(br.bits(3));
            while (rep--) lengths.push_back(0);
        } else {
            int rep = 11 + int(br.bits(7));
            while (rep--) lengths.push_back(0);
        }
    }
    if (int(lengths.size()) != hlit + hdist)
        throw std::runtime_error("inflate: code length overrun");
    lit.build(std::vector<int>(lengths.begin(), lengths.begin() + hlit));
    dist.build(std::vector<int>(lengths.begin() + hlit, lengths.end()));
}

}

std::vector<uint8_t> raw_inflate(const uint8_t* data, size_t size) {
    BitReader br(data, size);
    std::vector<uint8_t> out;
    Huffman flit = fixed_lit(), fdist = fixed_dist();
    for (;;) {
        int final = br.bit();
        int type = int(br.bits(2));
        if (type == 0) {
            br.align();
            uint8_t b0 = br.byte(), b1 = br.byte();
            uint16_t len = uint16_t(b0 | b1 << 8);
            br.byte();
            br.byte();
            for (int i = 0; i < len; ++i) out.push_back(br.byte());
        } else if (type == 1) {
            inflate_block_body(br, flit, fdist, out);
        } else if (type == 2) {
            Huffman lit, dist;
            read_dynamic(br, lit, dist);
            inflate_block_body(br, lit, dist, out);
        } else {
            throw std::runtime_error("inflate: reserved block type");
        }
        if (final) break;
    }
    return out;
}

std::vector<uint8_t> zlib_inflate(const uint8_t* data, size_t size) {
    if (size < 2) throw std::runtime_error("zlib: too small");
    uint8_t cmf = data[0], flg = data[1];
    if ((cmf & 0x0F) != 8) throw std::runtime_error("zlib: not deflate");
    if (((cmf << 8) | flg) % 31 != 0) throw std::runtime_error("zlib: bad check");
    size_t start = 2;
    if (flg & 0x20) start += 4;
    return raw_inflate(data + start, size - start);
}

}
