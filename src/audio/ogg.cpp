#include "omc/audio/ogg.hpp"

#include <array>

namespace omc::audio::ogg {

namespace {

const std::array<std::uint32_t, 256>& crc_table() {
    static const std::array<std::uint32_t, 256> table = [] {
        std::array<std::uint32_t, 256> t{};
        for (std::uint32_t i = 0; i < 256; ++i) {
            std::uint32_t r = i << 24;
            for (int k = 0; k < 8; ++k)
                r = (r & 0x80000000u) ? (r << 1) ^ 0x04c11db7u : (r << 1);
            t[i] = r;
        }
        return t;
    }();
    return table;
}

std::uint32_t rd32(const std::uint8_t* p) {
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}
std::int64_t rd64s(const std::uint8_t* p) {
    std::uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v |= static_cast<std::uint64_t>(p[i]) << (8 * i);
    return static_cast<std::int64_t>(v);
}

}

std::uint32_t crc32(const std::uint8_t* data, std::size_t n) {
    const auto& t = crc_table();
    std::uint32_t crc = 0;
    for (std::size_t i = 0; i < n; ++i)
        crc = (crc << 8) ^ t[((crc >> 24) ^ data[i]) & 0xff];
    return crc;
}

Demux demux(const std::vector<std::uint8_t>& bytes) {
    Demux out;
    const std::uint8_t* base = bytes.data();
    const std::size_t total = bytes.size();

    struct RawPage {
        Page meta;
        std::size_t header_off = 0;
        std::vector<std::uint8_t> lacing;
    };
    std::vector<RawPage> raw;

    std::size_t pos = 0;
    while (pos + 27 <= total) {
        if (!(base[pos] == 'O' && base[pos + 1] == 'g' && base[pos + 2] == 'g' &&
              base[pos + 3] == 'S')) {
            throw OggError("ogg: bad capture pattern");
        }
        RawPage rp;
        rp.meta.version = base[pos + 4];
        rp.meta.header_type = base[pos + 5];
        rp.meta.granule = rd64s(base + pos + 6);
        rp.meta.serial = rd32(base + pos + 14);
        rp.meta.sequence = rd32(base + pos + 18);
        rp.meta.crc_stored = rd32(base + pos + 22);
        const std::size_t seg_count = base[pos + 26];
        rp.meta.segment_count = seg_count;

        const std::size_t seg_table = pos + 27;
        if (seg_table + seg_count > total) throw OggError("ogg: truncated segment table");

        std::size_t body = 0;
        rp.lacing.resize(seg_count);
        for (std::size_t i = 0; i < seg_count; ++i) {
            rp.lacing[i] = base[seg_table + i];
            body += rp.lacing[i];
        }
        rp.meta.body_size = body;

        const std::size_t body_off = seg_table + seg_count;
        if (body_off + body > total) throw OggError("ogg: truncated page body");
        rp.header_off = body_off;

        const std::size_t page_len = 27 + seg_count + body;
        std::vector<std::uint8_t> tmp(base + pos, base + pos + page_len);
        tmp[22] = tmp[23] = tmp[24] = tmp[25] = 0;
        rp.meta.crc_computed = crc32(tmp.data(), tmp.size());
        rp.meta.crc_ok = (rp.meta.crc_computed == rp.meta.crc_stored);

        raw.push_back(std::move(rp));
        pos = body_off + body;
    }
    if (raw.empty()) throw OggError("ogg: no pages");

    for (const RawPage& rp : raw) {
        out.pages.push_back(rp.meta);
        if (rp.meta.crc_ok) ++out.crc_ok; else ++out.crc_fail;
    }

    out.serial = raw.front().meta.serial;

    std::vector<std::uint8_t> cur;
    bool building = false;
    for (const RawPage& rp : raw) {
        if (rp.meta.serial != out.serial) continue;
        out.last_granule = rp.meta.granule;

        const bool continuation = (rp.meta.header_type & 0x01) != 0;
        if (!continuation && building) {
            cur.clear();
            building = false;
        }

        std::size_t seg_data = rp.header_off;
        for (std::size_t i = 0; i < rp.lacing.size(); ++i) {
            const std::uint8_t lv = rp.lacing[i];
            cur.insert(cur.end(), base + seg_data, base + seg_data + lv);
            seg_data += lv;
            if (lv < 255) {
                Packet pk;
                pk.data = std::move(cur);
                const bool last_seg = (i + 1 == rp.lacing.size());
                pk.granule = last_seg ? rp.meta.granule : -1;
                out.packets.push_back(std::move(pk));
                cur.clear();
                building = false;
            } else {
                building = true;
            }
        }
    }

    return out;
}

}
