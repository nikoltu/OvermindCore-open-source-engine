#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace sc2::render {

enum class AudioChannel { UI, Unit, Combat, Ambient };

struct MixerVoice {
    std::string name;
    AudioChannel channel = AudioChannel::Unit;
    float gain = 1.0f;
    std::uint32_t unit = 0;
    int priority = 0;
};

class AudioMixer {
public:
    float master = 1.0f;
    float channel_volume[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    float falloff = 40.0f;
    float dedupe_window = 0.1f;
    int max_voices[4] = {4, 8, 8, 4};

    void begin_frame() { voices_.clear(); }

    bool play(const std::string& name, AudioChannel channel, int priority, float distance,
              std::uint32_t unit = 0) {
        const int ch = int(channel);
        const float atten = distance <= 0 ? 1.0f : 1.0f - distance / falloff;
        const float gain = master * channel_volume[ch] * (atten < 0 ? 0.0f : atten);
        if (gain <= 0.0f) return false;
        if (cooldown_.count(name) && cooldown_[name] > 0.0f) return false;

        int count = 0, weakest = -1;
        int weakest_pri = priority;
        for (std::size_t i = 0; i < voices_.size(); ++i) {
            if (int(voices_[i].channel) != ch) continue;
            ++count;
            if (voices_[i].priority < weakest_pri) {
                weakest_pri = voices_[i].priority;
                weakest = int(i);
            }
        }
        if (count >= max_voices[ch]) {
            if (weakest < 0) return false;
            voices_.erase(voices_.begin() + weakest);
        }
        cooldown_[name] = dedupe_window;
        voices_.push_back(MixerVoice{name, channel, gain, unit, priority});
        return true;
    }

    void advance(float dt) {
        for (auto it = cooldown_.begin(); it != cooldown_.end();) {
            it->second -= dt;
            if (it->second <= 0.0f)
                it = cooldown_.erase(it);
            else
                ++it;
        }
    }

    const std::vector<MixerVoice>& voices() const { return voices_; }
    std::size_t voice_count() const { return voices_.size(); }

private:
    std::vector<MixerVoice> voices_;
    std::unordered_map<std::string, float> cooldown_;
};

}
