#pragma once

#include <cstdint>
#include <stdexcept>
#include <vector>

namespace sc2::audio {

struct VorbisIdent {
    int channels = 0;
    std::uint32_t rate = 0;
    int blocksize0 = 0;
    int blocksize1 = 0;
};

struct VorbisPcm {
    int channels = 0;
    std::uint32_t rate = 0;
    std::vector<std::int16_t> interleaved;
    std::int64_t final_granule = -1;
    std::size_t frames() const {
        return channels ? interleaved.size() / static_cast<std::size_t>(channels) : 0;
    }
};

struct VorbisError : std::runtime_error {
    using std::runtime_error::runtime_error;
};
struct VorbisUnsupported : VorbisError {
    using VorbisError::VorbisError;
};

VorbisIdent probe_ogg_vorbis(const std::vector<std::uint8_t>& ogg_bytes);

VorbisPcm decode_ogg_vorbis(const std::vector<std::uint8_t>& ogg_bytes);

}
