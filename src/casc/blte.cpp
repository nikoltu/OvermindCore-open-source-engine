#include "omc/casc/blte.hpp"

#include <stdexcept>

#include "omc/core/byte_reader.hpp"
#include "omc/core/inflate.hpp"

namespace omc::casc {

using core::ByteReader;

namespace {

struct Chunk {
    uint32_t comp_size;
    uint32_t decomp_size;
};

void decode_chunk(const uint8_t* body_with_mode, size_t comp_size,
                  std::vector<uint8_t>& out) {
    if (comp_size == 0) throw std::runtime_error("blte: zero-length chunk");
    uint8_t mode = body_with_mode[0];
    const uint8_t* body = body_with_mode + 1;
    size_t body_len = comp_size - 1;
    switch (mode) {
        case 'N':
            out.insert(out.end(), body, body + body_len);
            break;
        case 'Z': {
            auto d = core::zlib_inflate(body, body_len);
            out.insert(out.end(), d.begin(), d.end());
            break;
        }
        case 'F': {
            static thread_local int blte_depth = 0;
            if (blte_depth >= 16) throw std::runtime_error("blte: nested deeper than 16");
            ++blte_depth;
            std::vector<uint8_t> d;
            try { d = blte_decode(body, body_len); } catch (...) { --blte_depth; throw; }
            --blte_depth;
            out.insert(out.end(), d.begin(), d.end());
            break;
        }
        case 'E':
            throw BlteEncrypted{out};
        default:
            throw std::runtime_error("blte: unknown chunk mode");
    }
}

}

std::vector<uint8_t> blte_decode(const uint8_t* data, size_t size) {
    ByteReader r(data, size);
    if (size < 8 || r.u8() != 'B' || r.u8() != 'L' || r.u8() != 'T' ||
        r.u8() != 'E')
        throw std::runtime_error("blte: bad magic");
    uint32_t header_size = r.u32be();

    std::vector<Chunk> chunks;
    size_t data_start;
    if (header_size > 0) {
        r.u8();
        uint32_t count = r.u24be();
        chunks.reserve(count);
        for (uint32_t i = 0; i < count; ++i) {
            Chunk c;
            c.comp_size = r.u32be();
            c.decomp_size = r.u32be();
            r.skip(16);
            chunks.push_back(c);
        }
        data_start = header_size;
    } else {
        chunks.push_back({uint32_t(size - 8), 0});
        data_start = 8;
    }

    std::vector<uint8_t> out;
    size_t p = data_start;
    for (const Chunk& c : chunks) {
        if (p + c.comp_size > size) throw std::runtime_error("blte: chunk overrun");
        decode_chunk(data + p, c.comp_size, out);
        p += c.comp_size;
    }
    return out;
}

std::vector<uint8_t> blte_decode(const std::vector<uint8_t>& data) {
    return blte_decode(data.data(), data.size());
}

}
