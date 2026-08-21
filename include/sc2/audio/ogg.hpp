#pragma once

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <vector>

namespace sc2::audio::ogg {

std::uint32_t crc32(const std::uint8_t* data, std::size_t n);

struct Page {
    std::uint8_t version = 0;
    std::uint8_t header_type = 0;
    std::int64_t granule = 0;
    std::uint32_t serial = 0;
    std::uint32_t sequence = 0;
    std::uint32_t crc_stored = 0;
    std::uint32_t crc_computed = 0;
    bool crc_ok = false;
    std::size_t segment_count = 0;
    std::size_t body_size = 0;
};

struct Packet {
    std::vector<std::uint8_t> data;
    std::int64_t granule = -1;
};

struct Demux {
    std::vector<Page> pages;
    std::vector<Packet> packets;
    std::uint32_t serial = 0;
    std::size_t crc_ok = 0;
    std::size_t crc_fail = 0;
    std::int64_t last_granule = -1;
};

struct OggError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

Demux demux(const std::vector<std::uint8_t>& bytes);

}
