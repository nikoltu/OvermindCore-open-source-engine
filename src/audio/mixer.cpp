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
        const double pan = v.pan < -1.0f ? -1.0 : (v.pan > 1.0f ? 1.0 : static_cast<double>(v.pan));
        const double theta = (pan + 1.0) * 0.78539816339744831;
        const double gl = std::cos(theta) * 1.4142135623730951, gr = std::sin(theta) * 1.4142135623730951;
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
                if (oc == 2) {
                    acc[base] += interp(0) * g * gl;
                    acc[base + 1] += interp(1) * g * gr;
                } else {
                    for (int c = 0; c < oc; ++c) acc[base + static_cast<std::size_t>(c)] += interp(c) * g;
                }
            } else if (sc == 1 && oc == 2) {
                const double s = interp(0) * g;
                acc[base] += s * gl;
                acc[base + 1] += s * gr;
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
    if (!decode_wav(d, out))
        throw std::runtime_error("audio::read_wav: unsupported format (16-bit PCM or IMA ADPCM only): " + path);
    return out;
}

static const std::int16_t kImaStep[89] = {
    7,     8,     9,     10,    11,    12,    13,    14,    16,    17,    19,    21,    23,    25,    28,
    31,    34,    37,    41,    45,    50,    55,    60,    66,    73,    80,    88,    97,    107,   118,
    130,   143,   157,   173,   190,   209,   230,   253,   279,   307,   337,   371,   408,   449,   494,
    544,   598,   658,   724,   796,   876,   963,   1060,  1166,  1282,  1411,  1552,  1707,  1878,  2066,
    2272,  2499,  2749,  3024,  3327,  3660,  4026,  4428,  4871,  5358,  5894,  6484,  7132,  7845,  8630,
    9493,  10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767};
static const int kImaIndex[16] = {-1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8};

bool decode_wav(const std::vector<std::uint8_t>& d, WavData& out) {
    out = WavData{};
    auto le16 = [&](std::size_t o) { return std::uint16_t(d[o] | (d[o + 1] << 8)); };
    auto le32 = [&](std::size_t o) {
        return std::uint32_t(d[o]) | (std::uint32_t(d[o + 1]) << 8) | (std::uint32_t(d[o + 2]) << 16) |
               (std::uint32_t(d[o + 3]) << 24);
    };
    auto tag = [&](std::size_t o, const char* t) {
        return o + 4 <= d.size() && d[o] == std::uint8_t(t[0]) && d[o + 1] == std::uint8_t(t[1]) &&
               d[o + 2] == std::uint8_t(t[2]) && d[o + 3] == std::uint8_t(t[3]);
    };
    if (d.size() < 12 || !tag(0, "RIFF") || !tag(8, "WAVE")) return false;
    std::uint16_t fmt_tag = 0, bits = 0, block_align = 0, spb = 0;
    std::uint32_t fact = 0;
    bool have_fmt = false, have_fact = false;
    std::size_t data_at = 0, data_len = 0;
    std::size_t p = 12;
    while (p + 8 <= d.size()) {
        const std::uint32_t csize = le32(p + 4);
        const std::size_t body = p + 8;
        if (body + csize > d.size()) break;
        if (tag(p, "fmt ") && csize >= 16) {
            fmt_tag = le16(body);
            out.channels = int(le16(body + 2));
            out.rate = le32(body + 4);
            block_align = le16(body + 12);
            bits = le16(body + 14);
            if (csize >= 20) spb = le16(body + 18);
            have_fmt = true;
        } else if (tag(p, "fact") && csize >= 4) {
            fact = le32(body);
            have_fact = true;
        } else if (tag(p, "data")) {
            data_at = body;
            data_len = csize;
        }
        p = body + csize + (csize & 1u);
    }
    if (!have_fmt || data_at == 0 || out.channels < 1) {
        out = WavData{};
        return false;
    }
    const int ch = out.channels;
    if (fmt_tag == 1 && bits == 16) {
        const std::size_t n = data_len / 2;
        out.samples.resize(n);
        for (std::size_t i = 0; i < n; ++i) out.samples[i] = std::int16_t(le16(data_at + i * 2));
        return true;
    }
    if (fmt_tag == 0x11 && bits == 4 && block_align >= std::uint16_t(4 * ch)) {
        const std::size_t hdr = std::size_t(4 * ch);
        const std::size_t per_block = spb ? spb : 1 + (std::size_t(block_align) - hdr) * 2 / std::size_t(ch);
        const std::size_t blocks = data_len / block_align;
        out.samples.reserve(blocks * per_block * std::size_t(ch));
        std::vector<int> pred(static_cast<std::size_t>(ch)), index(static_cast<std::size_t>(ch));
        std::vector<std::vector<std::int16_t>> chan(static_cast<std::size_t>(ch));
        for (std::size_t b = 0; b < blocks; ++b) {
            const std::size_t base = data_at + b * block_align;
            for (int c = 0; c < ch; ++c) {
                pred[std::size_t(c)] = std::int16_t(le16(base + std::size_t(4 * c)));
                int ix = d[base + std::size_t(4 * c) + 2];
                index[std::size_t(c)] = ix > 88 ? 88 : ix;
                chan[std::size_t(c)].assign(1, std::int16_t(pred[std::size_t(c)]));
            }
            const std::size_t nibbles = (per_block - 1) * std::size_t(ch);
            for (std::size_t k = 0; k < nibbles; ++k) {
                const std::size_t byte = base + hdr + (k >> 1);
                if (byte >= base + block_align) break;
                const int code = (k & 1) ? (d[byte] >> 4) & 0x0F : d[byte] & 0x0F;
                const std::size_t c = ch == 1 ? 0 : (k / 8) % std::size_t(ch);
                const int step = kImaStep[index[c]];
                int diff = step >> 3;
                if (code & 4) diff += step;
                if (code & 2) diff += step >> 1;
                if (code & 1) diff += step >> 2;
                int v = (code & 8) ? pred[c] - diff : pred[c] + diff;
                v = v < -32768 ? -32768 : (v > 32767 ? 32767 : v);
                pred[c] = v;
                int ni = index[c] + kImaIndex[code];
                index[c] = ni < 0 ? 0 : (ni > 88 ? 88 : ni);
                chan[c].push_back(std::int16_t(v));
            }
            std::size_t frames = chan[0].size();
            for (const auto& cv : chan) frames = cv.size() < frames ? cv.size() : frames;
            for (std::size_t f = 0; f < frames; ++f)
                for (int c = 0; c < ch; ++c) out.samples.push_back(chan[std::size_t(c)][f]);
        }
        if (have_fact && std::size_t(fact) * std::size_t(ch) < out.samples.size())
            out.samples.resize(std::size_t(fact) * std::size_t(ch));
        return true;
    }
    out = WavData{};
    return false;
}

}
