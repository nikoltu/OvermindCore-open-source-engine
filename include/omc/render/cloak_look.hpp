#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "omc/formats/m3.hpp"

namespace omc::render {

enum class CloakView : std::uint8_t { None = 0, Ally = 1, Detected = 2, Enemy = 3 };

const char* cloak_view_name(CloakView v);

struct CloakLook {
    float opacity = 1.0f;
    float strength = 0.0f;
};

class CloakModelLook {
public:
    using MaskLoader = std::function<std::vector<float>(const std::string& image_path)>;

    bool bind(const formats::Model* model, const MaskLoader& load_mask);
    bool valid() const { return model_ != nullptr && composite_ >= 0; }
    const formats::Model* model() const { return model_; }
    int sequence(const std::string& comma_tokens) const;
    float length_ms(int seq) const;
    CloakLook sample(int seq, float time_ms) const;

private:
    struct Child {
        int material = -1;
        bool displacement = false;
        bool cutout = false;
        float cutout_level = 0.0f;
        int mask = -1;
        std::uint16_t alpha_interp = 0, alpha_flags = 0;
        std::uint32_t alpha_id = 0;
        float alpha = 1.0f;
    };
    float coverage(const Child& c, int seq, float time_ms) const;

    const formats::Model* model_ = nullptr;
    int composite_ = -1;
    std::vector<Child> children_;
    std::vector<std::vector<float>> masks_;
};

class CloakTransitions {
public:
    using StateLookup = std::function<bool(CloakView from, CloakView to, std::string& enter, std::string& loop)>;

    struct Entry {
        CloakView view = CloakView::None;
        std::uint64_t frame = 0;
        bool seen = false;
        bool entering = false;
        int enter_seq = -1;
        int loop_seq = -1;
        double start_ms = 0.0;
        float enter_ms = 0.0f;
        float loop_ms = 0.0f;
        std::string enter;
        std::string loop;
    };
    struct Out {
        bool cloak_fx = false;
        bool entering = false;
        int seq = -1;
        float time_ms = 0.0f;
        CloakLook look;
    };

    void bind(const CloakModelLook* look, StateLookup lookup, bool play_enter) {
        look_ = look;
        lookup_ = std::move(lookup);
        play_enter_ = play_enter;
    }
    void begin_frame() { ++frame_; }
    Out update(std::uint32_t id, CloakView view, double now_ms);
    const Entry* entry(std::uint32_t id) const {
        return id < entries_.size() && entries_[id].seen ? &entries_[id] : nullptr;
    }

private:
    const CloakModelLook* look_ = nullptr;
    StateLookup lookup_;
    bool play_enter_ = true;
    std::uint64_t frame_ = 1;
    std::vector<Entry> entries_;
};

}
