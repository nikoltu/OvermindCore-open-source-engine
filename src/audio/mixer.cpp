#include "sc2/audio/mixer.hpp"

#include <cmath>
#include <fstream>
#include <stdexcept>

namespace sc2::audio {

Mixer::Mixer(std::uint32_t out_rate, int out_channels)
    : out_rate_(out_rate), out_channels_(out_channels) {
    if (out_rate_ == 0) throw std::runtime_error("audio::Mixer: out_rate must be non-zero");
    if (out_channels_ != 1 && out_channels_ != 2)
        throw std::runtime_error("audio::Mixer: out_channels must be 1 or 2");
}

int Mixer::add_voice(const Voice& v) {
    voices_.push_back(v);
    return static_cast<int>(voices_.size()) - 1;
}

void Mixer::stop(int id) {
    if (id >= 0 && id < static_cast<int>(voices_.size())) voices_[id].active = false;
}

void Mixer::start(int id) {
    if (id >= 0 && id < static_cast<int>(voices_.size())) voices_[id].active = true;
}

bool Mixer::is_active(int id) const {
    return id >= 0 && id < static_cast<int>(voices_.size()) && voices_[id].active;
}

const Voice* Mixer::voice(int id) const {
    if (id < 0 || id >= static_cast<int>(voices_.size())) return nullptr;
    return &voices_[id];
}

namespace {

std::int16_t clamp_i16(double x) {
    double r = (x >= 0.0) ? std::floor(x + 0.5) : std::ceil(x - 0.5);
    if (r > 32767.0) r = 32767.0;
    if (r < -32768.0) r = -32768.0;
    return static_cast<std::int16_t>(r);
}

}

void Mixer::mix(std::int16_t* out, std::size_t out_frames) {
    const int oc = out_channels_;
    std::vector<double> acc(out_frames * static_cast<std::size_t>(oc), 0.0);

    for (Voice& v : voices_) {
        if (!v.active || v.samples == nullptr || v.frame_count == 0 || v.channels < 1)
            continue;

        const double step = static_cast<double>(v.src_rate) / static_cast<double>(out_rate_);
        const double g = static_cast<double>(v.gain);
        const std::size_t fc = v.frame_count;
        const int sc = v.channels;

        for (std::size_t f = 0; f < out_frames; ++f) {
            if (v.cursor >= static_cast<double>(fc)) {
                if (v.loop) {
                    v.cursor = std::fmod(v.cursor, static_cast<double>(fc));
                } else {
                    v.active = false;
                    break;
                }
            }

            const std::size_t i0 = static_cast<std::size_t>(v.cursor);
            const double frac = v.cursor - static_cast<double>(i0);
            std::size_t i1 = i0 + 1;
            if (i1 >= fc) i1 = v.loop ? 0 : i0;

            auto interp = [&](int c) -> double {
                const double a = static_cast<double>(v.samples[i0 * static_cast<std::size_t>(sc) + c]);
                const double b = static_cast<double>(v.samples[i1 * static_cast<std::size_t>(sc) + c]);
                return a + (b - a) * frac;
            };

            const std::size_t base = f * static_cast<std::size_t>(oc);
            if (sc == oc) {
                for (int c = 0; c < oc; ++c) acc[base + static_cast<std::size_t>(c)] += interp(c) * g;
            } else if (sc == 1 && oc == 2) {
                const double s = interp(0) * g;
                acc[base] += s;
                acc[base + 1] += s;
            } else if (sc == 2 && oc == 1) {
                acc[base] += (interp(0) + interp(1)) * 0.5 * g;
            } else {
                const double s = interp(0) * g;
                for (int c = 0; c < oc; ++c) acc[base + static_cast<std::size_t>(c)] += s;
            }

            v.cursor += step;
        }
    }

    for (std::size_t i = 0; i < acc.size(); ++i) out[i] = clamp_i16(acc[i]);
}

namespace {

void put_le16(std::vector<std::uint8_t>& out, std::uint16_t v) {
    out.push_back(static_cast<std::uint8_t>(v));
    out.push_back(static_cast<std::uint8_t>(v >> 8));
}

void put_le32(std::vector<std::uint8_t>& out, std::uint32_t v) {
    out.push_back(static_cast<std::uint8_t>(v));
    out.push_back(static_cast<std::uint8_t>(v >> 8));
    out.push_back(static_cast<std::uint8_t>(v >> 16));
    out.push_back(static_cast<std::uint8_t>(v >> 24));
}

void put_tag(std::vector<std::uint8_t>& out, const char tag[4]) {
    out.insert(out.end(), tag, tag + 4);
}

std::uint16_t get_le16(const std::vector<std::uint8_t>& d, std::size_t off) {
    return static_cast<std::uint16_t>(d[off] | (d[off + 1] << 8));
}

std::uint32_t get_le32(const std::vector<std::uint8_t>& d, std::size_t off) {
    return static_cast<std::uint32_t>(d[off]) | (static_cast<std::uint32_t>(d[off + 1]) << 8) |
           (static_cast<std::uint32_t>(d[off + 2]) << 16) |
           (static_cast<std::uint32_t>(d[off + 3]) << 24);
}

bool tag_is(const std::vector<std::uint8_t>& d, std::size_t off, const char tag[4]) {
    return d[off] == static_cast<std::uint8_t>(tag[0]) && d[off + 1] == static_cast<std::uint8_t>(tag[1]) &&
           d[off + 2] == static_cast<std::uint8_t>(tag[2]) && d[off + 3] == static_cast<std::uint8_t>(tag[3]);
}

}

void write_wav(const std::string& path, const std::vector<std::int16_t>& pcm,
               std::uint32_t rate, int channels) {
    if (channels != 1 && channels != 2)
        throw std::runtime_error("audio::write_wav: channels must be 1 or 2");
    if (pcm.size() % static_cast<std::size_t>(channels) != 0)
        throw std::runtime_error("audio::write_wav: sample count not a whole number of frames");

    const std::uint32_t data_bytes = static_cast<std::uint32_t>(pcm.size() * sizeof(std::int16_t));
    const std::uint16_t block_align = static_cast<std::uint16_t>(channels * 2);
    const std::uint32_t byte_rate = rate * block_align;

    std::vector<std::uint8_t> buf;
    buf.reserve(44 + data_bytes);
    put_tag(buf, "RIFF");
    put_le32(buf, 36 + data_bytes);
    put_tag(buf, "WAVE");
    put_tag(buf, "fmt ");
    put_le32(buf, 16);
    put_le16(buf, 1);
    put_le16(buf, static_cast<std::uint16_t>(channels));
    put_le32(buf, rate);
    put_le32(buf, byte_rate);
    put_le16(buf, block_align);
    put_le16(buf, 16);
    put_tag(buf, "data");
    put_le32(buf, data_bytes);
    for (std::int16_t s : pcm) put_le16(buf, static_cast<std::uint16_t>(s));

    std::ofstream f(path, std::ios::binary);
    if (!f) throw std::runtime_error("audio::write_wav: cannot open " + path);
    f.write(reinterpret_cast<const char*>(buf.data()), static_cast<std::streamsize>(buf.size()));
    if (!f) throw std::runtime_error("audio::write_wav: write failed for " + path);
}

WavData read_wav(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) throw std::runtime_error("audio::read_wav: cannot open " + path);
    std::vector<std::uint8_t> d((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());

    if (d.size() < 12 || !tag_is(d, 0, "RIFF") || !tag_is(d, 8, "WAVE"))
        throw std::runtime_error("audio::read_wav: not a RIFF/WAVE file: " + path);

    WavData out;
    bool have_fmt = false, have_data = false;
    std::uint16_t fmt_tag = 0, bits = 0;

    std::size_t p = 12;
    while (p + 8 <= d.size()) {
        const std::uint32_t csize = get_le32(d, p + 4);
        const std::size_t body = p + 8;
        if (body + csize > d.size())
            throw std::runtime_error("audio::read_wav: truncated chunk in " + path);

        if (tag_is(d, p, "fmt ") && csize >= 16) {
            fmt_tag = get_le16(d, body + 0);
            out.channels = static_cast<int>(get_le16(d, body + 2));
            out.rate = get_le32(d, body + 4);
            bits = get_le16(d, body + 14);
            have_fmt = true;
        } else if (tag_is(d, p, "data")) {
            const std::size_t n = csize / sizeof(std::int16_t);
            out.samples.resize(n);
            for (std::size_t i = 0; i < n; ++i)
                out.samples[i] = static_cast<std::int16_t>(get_le16(d, body + i * 2));
            have_data = true;
        }
        p = body + csize + (csize & 1u);
    }

    if (!have_fmt) throw std::runtime_error("audio::read_wav: missing fmt chunk in " + path);
    if (!have_data) throw std::runtime_error("audio::read_wav: missing data chunk in " + path);
    if (fmt_tag != 1 || bits != 16)
        throw std::runtime_error("audio::read_wav: only 16-bit PCM is supported: " + path);

    return out;
}

}
