#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace omc::audio {

struct Voice {
    const int16_t* samples = nullptr;
    std::size_t frame_count = 0;
    std::uint32_t src_rate = 0;
    int channels = 1;
    float gain = 1.0f;
    float pan = 0.0f;
    bool loop = false;
    double cursor = 0.0;
    bool active = true;
};

class Mixer {
public:
    explicit Mixer(std::uint32_t out_rate, int out_channels = 2);

    int add_voice(const Voice& v);

    void stop(int id);
    void start(int id);
    bool is_active(int id) const;
    const Voice* voice(int id) const;
    std::size_t voice_count() const { return voices_.size(); }

    void mix(std::int16_t* out, std::size_t out_frames);

    std::uint32_t out_rate() const { return out_rate_; }
    int out_channels() const { return out_channels_; }

private:
    std::uint32_t out_rate_;
    int out_channels_;
    std::vector<Voice> voices_;
};

struct WavData {
    std::vector<std::int16_t> samples;
    std::uint32_t rate = 0;
    int channels = 0;
    std::size_t frames() const {
        return channels ? samples.size() / static_cast<std::size_t>(channels) : 0;
    }
};

void write_wav(const std::string& path, const std::vector<std::int16_t>& pcm,
               std::uint32_t rate, int channels);

WavData read_wav(const std::string& path);

bool decode_wav(const std::vector<std::uint8_t>& bytes, WavData& out);

}
